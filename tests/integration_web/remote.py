"""Stateless execution gateways: isolated runtimes, receipts and main ownership."""

import base64
import concurrent.futures
import contextlib
import hashlib
import io
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tarfile
import time
import uuid
from urllib.parse import urlencode

from integration_support import Server, assert_true, base_env, budget, event, tool_call, wait_until
from session_support import remove_runtime
from web_support import web_host

from integration_web.terminal import Socket, listed


def exchange(path, request, *, abandon=False):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(budget(15))
        peer.connect(str(path))
        data = json.dumps(request)
        # Exercise the same bounded chunk framing as the main client.
        for at in range(0, len(data), 65536):
            peer.sendall(
                (
                    json.dumps(
                        {"v": 2, "data": data[at : at + 65536], "end": at + 65536 >= len(data)}
                    )
                    + "\n"
                ).encode()
            )
        if abandon:
            return
        chunks = []
        with peer.makefile("r") as stream:
            for line in stream:
                frame = json.loads(line)
                chunks.append(frame["data"])
                if frame["end"]:
                    return json.loads("".join(chunks))
        raise AssertionError("gateway closed before response")


@contextlib.contextmanager
def gateway(binary, home, folder, client=None, runtime=None):
    client, runtime = client or uuid.uuid4().hex, runtime or uuid.uuid4().hex
    auth = uuid.uuid4().hex
    startup = json.dumps({"auth": auth, "sandbox": "off"}) + "\n"
    env = base_env(home, "http://127.0.0.1:1")
    # Provider config must not be loaded, even when one already exists.
    env["UAGENT_SANDBOX"] = "invalid-remote-env-must-not-be-used"
    started = subprocess.run(
        [binary, "--execution-start", client, runtime, str(folder)],
        env=env,
        input=startup,
        capture_output=True,
        text=True,
        timeout=budget(20),
    )
    assert_true(started.returncode == 0, started.stderr + started.stdout)
    identity = json.loads(started.stdout)
    owner = pathlib.Path(identity["socket"]).parent / "owner"
    pid = int(owner.read_text().split()[0])
    try:
        identity["auth"] = auth
        yield (
            identity,
            lambda request, **kwargs: exchange(
                identity["socket"], {**request, "auth": auth}, **kwargs
            ),
        )
    finally:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        wait_until(
            lambda: not pathlib.Path(identity["socket"]).exists(),
            timeout=budget(10),
            message="gateway did not stop",
        )


def open_tools(call):
    value = call({"op": "open", "sandbox": "off"})
    assert_true("error" not in value, value)
    return value


def run(call, command, **fields):
    return call(
        {
            "op": "run",
            "tool": "run",
            "id": uuid.uuid4().hex,
            "args": {"command": command, "description": "Gateway test", "yield_ms": 10000},
            "timeout_s": 10,
            "approval": "yolo",
            **fields,
        }
    )


def test_execution_start_is_stateless_and_reuses_runtime(root, home, *, binary):
    folder = root / "project"
    folder.mkdir()
    (folder / "hello.txt").write_text("remote bytes")
    client, runtime = uuid.uuid4().hex, uuid.uuid4().hex
    with gateway(binary, home, folder, client, runtime) as (identity, call):
        assert_true(
            exchange(identity["socket"], {"op": "ping"}).get("error"),
            "unauthenticated gateway accepted request",
        )
        assert_true(
            "before-open" in run(call, "printf before-open")["output"], "tools need an initial open"
        )
        tools = open_tools(call)
        names = {tool["name"] for tool in tools["tools"]}
        assert_true({"run", "read_path", "write_file", "activity"} <= names, names)
        assert_true(not names & {"memory", "todo", "browser", "uagent"}, names)
        assert_true(
            "remote bytes"
            in call({"op": "run", "tool": "read_path", "args": {"path": "hello.txt"}})["output"],
            "read failed",
        )
        assert_true("gateway-ok" in run(call, "printf gateway-ok")["output"], "run failed")
        started = subprocess.run(
            [binary, "--execution-start", client, runtime, str(folder)],
            env=base_env(home, "http://127.0.0.1:1"),
            input=json.dumps({"auth": identity["auth"], "sandbox": "off"}) + "\n",
            capture_output=True,
            text=True,
            timeout=budget(10),
        )
        assert_true(json.loads(started.stdout)["socket"] == identity["socket"], "runtime replaced")
        assert_true(not (home / ".uagent").exists(), "gateway wrote durable application state")
        assert_true(
            call({"op": "run", "tool": "models", "args": {}}).get("error"), "provider RPC exposed"
        )
    remove_runtime(home)


def test_execution_receipts_do_not_repeat_uncertain_mutations(root, home, *, binary):
    with gateway(binary, home, root) as (_, call):
        open_tools(call)
        request = {
            "op": "run",
            "id": uuid.uuid4().hex,
            "tool": "run",
            "approval": "yolo",
            "timeout_s": 10,
            "args": {
                "command": "sleep 0.2; printf x >> once.txt; printf done",
                "description": "Once",
                "yield_ms": 10000,
            },
        }
        call(request, abandon=True)
        receipt = None

        def done():
            nonlocal receipt
            receipt = call({"op": "result", "id": request["id"]})
            return receipt.get("done")

        wait_until(done, timeout=budget(10), message="lost response had no receipt")
        assert_true("done" in receipt["result"]["output"], receipt)
        assert_true(call(request) == receipt["result"], "duplicate changed result")
        assert_true((root / "once.txt").read_text() == "x", "mutation ran twice")


def test_execution_receipts_evict_oldest_completed_request(root, home, *, binary):
    with gateway(binary, home, root) as (_, call):
        for index in range(256):
            call({"op": "ping", "id": f"{index + 16:032x}"})
        recent = f"{1:032x}"
        call({"op": "ping", "id": recent})
        call({"op": "ping", "id": f"{2:032x}"})
        assert_true(
            call({"op": "result", "id": recent})["known"], "recent receipt evicted by key order"
        )
        assert_true(not call({"op": "result", "id": f"{16:032x}"})["known"], "old receipt retained")


def test_execution_clients_have_isolated_cwd_cancellation_and_terminals(root, home, *, binary):
    left, right = root / "left", root / "right"
    left.mkdir()
    right.mkdir()
    with gateway(binary, home, left) as (a, first), gateway(binary, home, right) as (b, second):
        open_tools(first)
        open_tools(second)
        assert_true(a["socket"] != b["socket"], "workers shared")
        assert_true(
            exchange(b["socket"], {"op": "ping", "auth": a["auth"]}).get("error"),
            "another main credential reached this worker",
        )
        request = {
            "op": "run",
            "id": uuid.uuid4().hex,
            "tool": "run",
            "approval": "yolo",
            "timeout_s": 10,
            "args": {"command": "sleep 8", "description": "Cancel me", "yield_ms": 10000},
        }
        with concurrent.futures.ThreadPoolExecutor() as pool:
            pending = pool.submit(first, request)
            wait_until(
                lambda: first({"op": "result", "id": request["id"]}).get("known"),
                timeout=budget(3),
                message="command not started",
            )
            assert_true(
                not second({"op": "cancel", "id": request["id"]})["known"],
                "other client could cancel",
            )
            before = time.monotonic()
            first({"op": "cancel", "id": request["id"]})
            result = pending.result(timeout=budget(3))
            assert_true(time.monotonic() - before < budget(3) and result["status"] == 2, result)
        assert_true(str(right) in run(second, "pwd")["output"], "cwd crossed clients")
        shell = second(
            {"op": "terminal", "action": "open", "folder": str(root), "id": uuid.uuid4().hex}
        )["terminal"]
        viewer = uuid.uuid4().hex

        def terminal(action, **fields):
            return second(
                {"op": "terminal", "action": action, "terminal": shell, "viewer": viewer, **fields}
            )

        assert_true(terminal("attach") == {}, "attach failed")
        terminal("write", data=b"printf 'terminal-survives\n'\n".hex())
        output = bytearray()

        def seen():
            output.extend(bytes.fromhex(terminal("read")["data"]))
            return b"terminal-survives" in output

        wait_until(seen, timeout=budget(10), message="terminal output missing")
        terminal("detach")
        viewer = uuid.uuid4().hex
        assert_true(terminal("attach") == {}, "terminal did not survive transport")
        terminal("close")


def test_execution_binary_files_revisions_and_remote_approval(root, home, *, binary):
    project = root / "project"
    project.mkdir()
    outside = root / "outside"
    outside.write_text("outside")
    (project / "link").symlink_to(outside)
    (project / "text").write_text("before")
    binary_bytes = bytes(range(256)) * 2048
    with gateway(binary, home, project) as (_, call):
        open_tools(call)
        checked = call(
            {"op": "check", "tool": "read_path", "approval": "ask", "args": {"path": "link"}}
        )
        assert_true(checked["approval"] != 0, checked)
        (project / "link").unlink()
        (project / "link").symlink_to(project / "text")
        refused = call(
            {
                "op": "run",
                "tool": "read_path",
                "id": uuid.uuid4().hex,
                "approval": "ask",
                "args": checked["args"],
                "scope": checked["scope"],
            }
        )
        assert_true(
            refused["status"] == 1 and "changed after approval" in refused["output"], refused
        )
        value = call(
            {
                "op": "run",
                "tool": "write_file",
                "id": uuid.uuid4().hex,
                "approval": "yolo",
                "args": {"path": "blob.bin", "content_hex": binary_bytes.hex()},
            }
        )
        assert_true(value["status"] == 0, value)
        raw = call({"op": "files", "mode": "raw", "path": "blob.bin"})
        assert_true(bytes.fromhex(raw["data"]) == binary_bytes, "binary changed")
        read = call({"op": "files", "mode": "read", "path": "text"})
        (project / "text").write_text("outside edit")
        conflict = call(
            {
                "op": "files",
                "mode": "write",
                "path": "text",
                "text": "overwrite",
                "revision": read["revision"],
            }
        )
        assert_true(conflict.get("conflict"), conflict)
        assert_true((project / "text").read_text() == "outside edit", "conflict overwritten")


def test_file_operations_match_local_http_and_execution_gateway(root, home, *, binary):
    project = root / "project"
    project.mkdir()
    (project / "note.txt").write_text("before")
    (project / "large.txt").write_text("x" + "é" * 600_000)
    (root / "secret.txt").write_text("outside")
    (project / "outside").symlink_to(root / "secret.txt")
    subprocess.run(["git", "init", "-q", str(project)], check=True, capture_output=True)
    with web_host(binary, root, home, "http://127.0.0.1:1") as (client, code, _, _):
        client.pair(code)
        session = client.create(project, activate=False)
        endpoint = f"/api/sessions/{session['id']}/files"
        with gateway(binary, home, project) as (_, call):
            for mode, path in (
                ("list", ""),
                ("read", "note.txt"),
                ("read", "large.txt"),
                ("find", "nt"),
                ("changes", ""),
                ("diff", "note.txt"),
                ("read", "missing.txt"),
                ("read", "../secret.txt"),
                ("read", "outside"),
            ):
                status, local, _ = client.json(endpoint + "?" + urlencode({mode: path}))
                remote = call({"op": "files", "mode": mode, "path": path})
                assert_true(status == remote.pop("status", 200) and local == remote, (mode, path))
            for transport in ("local", "remote"):
                read = call({"op": "files", "mode": "read", "path": "note.txt"})
                operation = {
                    "op": "files",
                    "mode": "write",
                    "path": "note.txt",
                    "text": f"saved through {transport}",
                    "revision": read["revision"],
                }
                for expected in (200, 409):
                    if transport == "local":
                        status, result, _ = client.json(endpoint, operation)
                    else:
                        result = call(operation)
                        status = result.get("status", 200)
                    assert_true(status == expected, (transport, result))
                assert_true((project / "note.txt").read_text() == operation["text"], "edit lost")
            # A deck's pages are moved where the folder is.
            (project / "01-a.html").write_text("a")
            (project / "02-b.html").write_text("b")
            moves = [
                {"from": "01-a.html", "to": "02-a.html"},
                {"from": "02-b.html", "to": "01-b.html"},
            ]
            moved = call({"op": "files", "mode": "move", "path": "", "moves": moves})
            pages = sorted(page.name for page in project.glob("*.html"))
            assert_true(moved == {"moved": 2} and pages == ["01-b.html", "02-a.html"], moved)


def test_remote_bundle_requires_all_matching_payloads(root, home, *, binary):
    bundle = root / "bundle"
    bundle.mkdir()
    env = base_env(home, "http://127.0.0.1:1")
    build = subprocess.check_output([binary, "--build-id"], env=env, text=True).strip()
    # The checker validates archive bytes; installation checks their contents.
    manifest = {"build": build, "targets": {}}
    for target in ("linux-x86_64", "linux-arm64", "macos-x86_64", "macos-arm64"):
        content = target.encode()
        (bundle / f"{target}.tar.gz").write_bytes(content)
        manifest["targets"][target] = {
            "bytes": len(content),
            "sha256": hashlib.sha256(content).hexdigest(),
        }

    def check(value, expected):
        (bundle / "manifest.json").write_text(json.dumps(value))
        result = subprocess.run(
            [binary, "--check-remote-bundle", str(bundle)], env=env, capture_output=True, text=True
        )
        assert_true((result.returncode == 0) == expected, result.stderr)

    check(manifest, True)
    check({**manifest, "build": "different-build"}, False)
    check({**manifest, "targets": []}, False)
    check({**manifest, "targets": dict(list(manifest["targets"].items())[:3])}, False)
    for target in manifest["targets"]:
        payload = bundle / f"{target}.tar.gz"
        content = payload.read_bytes()
        payload.write_bytes(b"!" + content[1:])
        check(manifest, False)
        payload.unlink()
        check(manifest, False)
        payload.write_bytes(content)
    check(manifest, True)


def test_remote_bundle_user_setting_and_environment_precedence(root, home, *, binary):
    build = subprocess.check_output([binary, "--build-id"], text=True).strip()
    bundle = root / "bundle"
    bundle.mkdir()
    payload = io.BytesIO()
    # Installation only needs a matching executable; no runtime is started.
    script = f"#!/bin/sh\nprintf '%s\\n' '{build}'\n".encode()
    with tarfile.open(fileobj=payload, mode="w:gz") as archive:
        member = tarfile.TarInfo("uagent")
        member.size, member.mode = len(script), 0o755
        archive.addfile(member, io.BytesIO(script))
    content = payload.getvalue()
    manifest = {"build": build, "targets": {}}
    for target in ("linux-x86_64", "linux-arm64", "macos-x86_64", "macos-arm64"):
        (bundle / f"{target}.tar.gz").write_bytes(content)
        manifest["targets"][target] = {
            "bytes": len(content),
            "sha256": hashlib.sha256(content).hexdigest(),
        }
    (bundle / "manifest.json").write_text(json.dumps(manifest))
    tools = root / "bin"
    tools.mkdir()
    source = pathlib.Path(__file__).resolve().parents[1] / "fixtures/ssh_relay.py"
    ssh = tools / "ssh"
    ssh.write_text(f"#!{sys.executable}\n" + source.read_text())
    ssh.chmod(0o700)
    settings = home / ".uagent/config/settings.json"
    settings.parent.mkdir(parents=True, exist_ok=True)
    for override in (False, True):
        remote_home = root / f"remote-{override}"
        remote_home.mkdir()
        (home / "ssh-fixture-home").write_text(str(remote_home))
        settings.write_text(
            json.dumps(
                {
                    "format": 3,
                    "all": {"web.remoteBundle": str(root / "missing" if override else bundle)},
                    "projects": {},
                }
            )
        )
        environment = {
            "PATH": f"{tools}:{os.environ['PATH']}",
            "UAGENT_REMOTE_BUNDLE": str(bundle) if override else None,
        }
        with web_host(binary, root, home, "http://127.0.0.1:1", extra_env=environment) as (
            web,
            code,
            _,
            _,
        ):
            web.pair(code)
            status, value, _ = web.json(
                "/api/hosts", {"action": "add", "address": f"peer-{override}"}
            )
            assert_true(status == 200, value)

            def connected(value=value):
                return any(
                    row["id"] == value["id"] and row["phase"] == "connected"
                    for row in web.json("/api/hosts")[1]["hosts"]
                )

            try:
                wait_until(
                    connected, timeout=budget(15), message="configured bundle was not installed"
                )
            except AssertionError:
                raise AssertionError(web.json("/api/hosts")[1]) from None
            assert_true(
                (remote_home / f".uagent/remote/builds/{build}/uagent").is_file(),
                "installation missing",
            )


def test_remote_auth_failure_stops_until_manual_reconnect(root, home, *, binary):
    tools = root / "bin"
    tools.mkdir()
    ssh = tools / "ssh"
    ssh.write_text(
        '#!/bin/sh\necho attempt >> "$HOME/ssh-attempts"\n'
        'echo "Permission denied (publickey)." >&2\nexit 255\n'
    )
    ssh.chmod(0o700)
    attempts = home / "ssh-attempts"
    with web_host(
        binary,
        root,
        home,
        "http://127.0.0.1:1",
        extra_env={"PATH": f"{tools}:{os.environ['PATH']}"},
    ) as (client, code, _, _):
        client.pair(code)
        # An address is one argument of ssh, never an option to it.
        refused = client.json("/api/hosts", {"action": "add", "address": "-oProxyCommand=x"})
        assert_true(refused[0] == 400 and not client.json("/api/hosts")[1]["hosts"], refused)
        status, value, _ = client.json("/api/hosts", {"action": "add", "address": "GPU"})
        assert_true(status == 200, value)
        host = value["id"]

        def failed():
            rows = client.json("/api/hosts")[1]["hosts"]
            return rows and rows[0]["phase"] == "disconnected" and rows[0]["error"]

        wait_until(failed, timeout=budget(10), message="authentication failure did not settle")
        assert_true("ssh-add" in failed(), failed())
        # A retry would report reconnecting; disconnected with an error is final.
        assert_true(len(attempts.read_text().splitlines()) == 1, "authentication was retried")
        client.json("/api/hosts", {"action": "reconnect", "id": host})
        wait_until(
            lambda: len(attempts.read_text().splitlines()) == 2,
            timeout=budget(10),
            message="manual reconnect did not start a fresh attempt",
        )


def test_remote_main_owns_provider_history_and_settings(root, home, *, binary):
    remote_home = root / "remote-home"
    remote_home.mkdir()
    project = root / "remote-project"
    project.mkdir()
    canary = root / "main-canary"
    canary.write_text("untouched")
    build = subprocess.check_output([binary, "--build-id"], text=True).strip()
    installed = remote_home / ".uagent/remote/builds" / build / "uagent"
    installed.parent.mkdir(parents=True)
    installed.symlink_to(binary)
    (home / "ssh-fixture-home").write_text(str(remote_home))
    tools = root / "bin"
    tools.mkdir()
    ssh = tools / "ssh"
    source = pathlib.Path(__file__).resolve().parents[1] / "fixtures/ssh_relay.py"
    ssh.write_text(f"#!{sys.executable}\n" + source.read_text())
    ssh.chmod(0o700)
    (project / "AGENTS.md").write_text("Remote workspace instruction canary.")
    (project / ".mcp.json").write_text(
        '{"mcpServers":{"blocked":{"command":"sh","args":["-c","touch forbidden"],"env":{"KEY":"${MAIN_SECRET}"}}}}'
    )
    (project / "pixel.png").write_bytes(
        base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aTp8AAAAASUVORK5CYII="
        )
    )

    main_copy = ""
    copied_bytes = bytes(range(256)) * 1024

    def copy_attachment(_, body):
        assert_true(main_copy in json.dumps(body), "main attachment reference missing")
        return tool_call("write_file", {"path": "copied.bin", "from": main_copy})

    def answer(_, body):
        assert_true(
            "Remote workspace instruction canary" in json.dumps(body),
            "remote project prompt missing",
        )
        return tool_call(
            "run",
            {
                "command": 'printf remote-command-ok > remote.txt; printf %s "${MAIN_SECRET-}${OPENROUTER_API_KEY-}${UAGENT_PROVIDERS-}" > remote-env; pwd',
                "description": "Write remote marker",
                "yield_ms": 10000,
            },
        )

    def received_image(_, body):
        assert_true('"image_url"' in json.dumps(body), "remote image did not reach main provider")
        return event({"content": "main-provider-complete"})

    environment = {
        "PATH": f"{tools}:{os.environ['PATH']}",
        "UAGENT_TEST_SSH_HOME": str(remote_home),
        "MAIN_SECRET": "main-private-canary",
        "OPENROUTER_API_KEY": "private-provider-key-canary",
    }
    with Server(
        [
            copy_attachment,
            # An empty `from` is none: the content is written.
            tool_call("write_file", {"path": "plain.txt", "content": "plain", "from": ""}),
            answer,
            tool_call("read_path", {"path": "pixel.png"}),
            received_image,
            tool_call(
                "run",
                {
                    "command": "printf restarted-worker > restarted.txt",
                    "description": "Check recovery",
                    "yield_ms": 10000,
                },
            ),
            event({"content": "worker-recovered"}),
        ]
    ) as provider:
        with web_host(binary, root, home, provider.url, extra_env=environment) as (web, code, _, _):
            web.pair(code)
            status, value, _ = web.json("/api/hosts", {"action": "add", "address": "peer"})
            assert_true(status == 200, value)
            host = value["id"]

            def phase():
                return web.json("/api/hosts")[1]["hosts"][0]["phase"]

            wait_until(
                lambda: phase() == "connected", timeout=budget(12), message="SSH did not connect"
            )
            measured = web.json(f"/api/hosts/{host}/ping")
            assert_true(measured[0] == 200 and measured[1]["round_trip_ms"] >= 0, measured)
            browsed = web.json("/api/folders?" + urlencode({"host": host, "path": str(project)}))
            assert_true(browsed[0] == 200 and browsed[1]["path"] == str(project.resolve()), browsed)
            assert_true(all(row["dir"] for row in browsed[1]["entries"]), browsed)
            session = web.command("create", cwd=str(project), host=host)["session"]
            assert_true(session["host"] == host, session)
            session = web.command("activate", session)["session"]
            web.until(session, lambda value: value["metadata"]["status"] == "idle")
            web.command("submit", session, text="/yolo")
            web.until(session, lambda value: value["state"].get("yolo", False))
            upload = web.json(
                f"/api/sessions/{session['id']}/attachments?name=main-copy.bin",
                raw=copied_bytes,
                headers={"Content-Type": "application/octet-stream"},
            )
            assert_true(upload[0] == 200, upload)
            draft = json.loads((home / f".uagent/web/drafts/{session['id']}.json").read_text())
            main_copy = draft["path"] + ".assets/" + upload[1]["id"] + ".data"
            web.command(
                "submit",
                session,
                text="Write a marker in the remote project",
                attachment_ids=[upload[1]["id"]],
            )
            web.until(session, lambda value: "main-provider-complete" in json.dumps(value))
            assert_true(
                (project / "remote.txt").read_text() == "remote-command-ok",
                "remote command missing",
            )
            assert_true(
                (project / "remote-env").read_text() == "",
                "main settings or secrets reached remote child",
            )
            assert_true(
                (project / "copied.bin").read_bytes() == copied_bytes,
                "main attachment copy changed",
            )
            assert_true(
                (project / "plain.txt").read_text() == "plain", "empty from was not a write"
            )
            assert_true(canary.read_text() == "untouched", "main canary changed")
            assert_true(not (project / "forbidden").exists(), "remote MCP ran")
            assert_true(any((home / ".uagent/history").rglob("*.json")), "history not main owned")
            assert_true(
                list((remote_home / ".uagent").iterdir()) == [remote_home / ".uagent/remote"],
                "remote acquired app state",
            )
            result = web.json(
                f"/api/sessions/{session['id']}/files?" + urlencode({"read": "remote.txt"})
            )[1]
            assert_true(result["text"] == "remote-command-ok", result)
            for text in ("first edit", "second edit"):
                saved = web.json(
                    f"/api/sessions/{session['id']}/files",
                    {
                        "path": "remote.txt",
                        "text": text,
                        "revision": result["revision"],
                        "id": "browser-must-not-own-rpc-receipts",
                    },
                )
                assert_true(saved[0] == 200, saved)
                result = web.json(f"/api/sessions/{session['id']}/files?read=remote.txt")[1]
                assert_true(result["text"] == text, result)

            # A worker can disappear while its SSH master remains healthy.
            def workers():
                rows = subprocess.check_output(["ps", "-axo", "pid=,args="], text=True)
                return [
                    int(fields[0])
                    for fields in (row.split(maxsplit=1) for row in rows.splitlines())
                    if len(fields) == 2
                    and "--execution-worker" in fields[1]
                    and str(project) in fields[1]
                ]

            for pid in workers():
                with contextlib.suppress(ProcessLookupError):
                    os.kill(pid, signal.SIGTERM)
            wait_until(
                lambda: not workers(), timeout=budget(10), message="killed worker did not exit"
            )
            # An open SSH listener can accept a write whose worker died. Its
            # outcome is uncertain, so recovery must not silently replay it.
            restarted = web.json(
                f"/api/sessions/{session['id']}/files",
                {"path": "remote.txt", "text": "after restart", "revision": result["revision"]},
            )
            assert_true(restarted[0] == 503, restarted)
            recovered_file = web.json(f"/api/sessions/{session['id']}/files?read=remote.txt")
            assert_true(recovered_file[0] == 200, recovered_file)
            assert_true(recovered_file[1]["text"] == "second edit", recovered_file)
            web.command("submit", session, text="Check the restarted worker")
            web.until(session, lambda value: "worker-recovered" in json.dumps(value))
            assert_true(
                (project / "restarted.txt").read_text() == "restarted-worker", "tools not restored"
            )
            forward_log = home / "ssh-fixture-forwards.json"
            baseline = len(json.loads(forward_log.read_text()))
            local = web.command("terminal", action="open", cwd=str(root))["result"]["id"]
            closed_local = web.command("terminal", action="close", id=local, host=host)
            assert_true(closed_local["accepted"], closed_local)
            for index in range(10):
                panel = web.command("terminal", action="open", cwd=str(project), host=host)[
                    "result"
                ]["id"]
                viewer = Socket(web, f"/api/terminals/{panel}")
                viewer.type("printf panel-ready\\n\n")
                viewer.until("panel-ready")
                if index % 3 == 0:
                    web.command("terminal", action="close", id=panel, host=host)
                elif index % 3 == 1:
                    viewer.type("exit\n")
                viewer.close()
                wait_until(
                    lambda panel=panel: all(row["id"] != panel for row in listed(web)),
                    timeout=budget(10),
                    message="panel terminal record leaked",
                )
                wait_until(
                    lambda: len(json.loads(forward_log.read_text())) == baseline,
                    timeout=budget(10),
                    message="terminal SSH forward leaked",
                )
            # Quiet checks never start a session, so their forwards have no
            # later runtime owner to clean them up.
            task = dict(
                name="Quiet remote check",
                prompt="Report changes",
                check="exit 1",
                cwd=str(project),
                host=host,
                environment="local",
                permissions="yolo",
                schedule=dict(type="interval", seconds=3600),
            )
            item = web.command("schedule", action="save", revision="", task=task)["result"]["item"]
            for _ in range(3):
                run_id = web.command("schedule", action="run", key=item["id"])["result"]["run"][
                    "id"
                ]

                def quiet(run_id=run_id):
                    runs = web.command("schedule", action="list")["result"]["runs"]
                    return any(row["id"] == run_id and row["status"] == "quiet" for row in runs)

                wait_until(quiet, timeout=budget(10), message="remote check did not settle")
                assert_true(
                    len(json.loads(forward_log.read_text())) == baseline,
                    "quiet schedule leaked a forward",
                )
            # An abandoned open has no websocket whose teardown can clean it.
            abandoned = web.command("terminal", action="open", cwd=str(project), host=host)[
                "result"
            ]["id"]
            expiry = time.monotonic() + 31
            wait_until(
                lambda: time.monotonic() >= expiry,
                timeout=budget(35),
                message="terminal first-viewer grace did not elapse",
            )
            terminal = web.command(
                "terminal", action="open", cwd=str(project), folder=str(project), host=host
            )["result"]["id"]
            assert_true(
                all(row["id"] != abandoned for row in listed(web)),
                "unviewed terminal record retained",
            )
            assert_true(
                len(json.loads(forward_log.read_text())) == baseline + 1,
                "unviewed terminal forward retained",
            )
            viewer = Socket(web, f"/api/terminals/{terminal}")
            viewer.type("export REMOTE_PTY_SURVIVAL=still-here\n")
            viewer.type('printf "before-restart-%s\\n" "$REMOTE_PTY_SURVIVAL"\n')
            viewer.until("before-restart-still-here")
            viewer.close()
            web.json("/api/hosts", {"action": "disconnect", "id": host})
            wait_until(
                lambda: phase() == "disconnected", timeout=budget(5), message="disconnect ignored"
            )
            assert_true(web.snapshot(session)["metadata"]["host"] == host, "offline history lost")
            assert_true(
                web.request(f"/api/hosts/{host}/ping")[0] == 503, "offline ping reconnected"
            )
            web.json("/api/hosts", {"action": "reconnect", "id": host})
            wait_until(
                lambda: phase() == "connected",
                timeout=budget(12),
                message="manual reconnect failed",
            )
            recovered = web.json(f"/api/sessions/{session['id']}/files?read=remote.txt")
            assert_true(recovered[0] == 200, recovered)
            viewer = Socket(web, f"/api/terminals/{terminal}")
            viewer.type('printf "after-disconnect-%s\\n" "$REMOTE_PTY_SURVIVAL"\n')
            output, _, closed = viewer.until("after-disconnect-still-here")
            assert_true(closed is None, output)
            viewer.close()
        with web_host(binary, root, home, provider.url, extra_env=environment) as (web, code, _, _):
            web.pair(code)
            assert_true(
                any(row["id"] == terminal for row in listed(web)),
                "main restart lost terminal catalogue",
            )
            web.json("/api/hosts", {"action": "reconnect", "id": host})
            wait_until(
                lambda: web.json("/api/hosts")[1]["hosts"][0]["phase"] == "connected",
                timeout=budget(12),
                message="restart reconnect failed",
            )
            viewer = Socket(web, f"/api/terminals/{terminal}")
            viewer.type('printf "after-restart-%s\\n" "$REMOTE_PTY_SURVIVAL"\n')
            output, _, closed = viewer.until("after-restart-still-here")
            assert_true(closed is None, output)
            viewer.close()
            web.command("terminal", action="close", id=terminal)
    # Main session teardown closes owned commands. Stop temporary idle workers
    # under this test HOME before its directory is removed.
    rows = subprocess.check_output(["ps", "-axo", "pid=,args="], text=True)
    for row in rows.splitlines():
        fields = row.split(maxsplit=1)
        if len(fields) == 2 and "--execution-worker" in fields[1] and str(project) in fields[1]:
            with contextlib.suppress(ProcessLookupError):
                os.kill(int(fields[0]), signal.SIGTERM)
    remove_runtime(remote_home)
