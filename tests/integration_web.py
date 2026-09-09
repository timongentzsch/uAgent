import base64
import json
import os
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path

from integration_support import (
    Server,
    assert_true,
    budget,
    event,
    run,
    tool_call,
    wait_until,
    write_json_response,
)
from web_support import WebClient, web_host

PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a5WQAAAAASUVORK5CYII="
)


def test_web_external_origin_pairing(root, home, *, binary):
    with Server([event({"content": "unused"})]) as provider:
        for origin in ("https://browser.example", "http://100.64.0.9:18080"):
            with web_host(
                binary, root, home, provider.url, extra_env={"UAGENT_WEB_ORIGIN": origin}
            ) as (client, code, _, env):
                client.origin = origin
                host = {"Host": origin.split("://", 1)[1]}
                status, value, headers = client.json("/api/auth", {"code": code}, headers=host)
                assert_true(status == 200, value)
                cookie = headers["Set-Cookie"]
                assert_true(("; Secure" in cookie) == origin.startswith("https://"), cookie)
                client.cookie = cookie.split(";", 1)[0]
                assert_true(client.json("/api/sessions", headers=host)[0] == 200, "pairing failed")
                assert_true(
                    client.json("/api/sessions", headers={**host, "Origin": "http://evil.example"})[
                        0
                    ]
                    == 403,
                    "foreign origin accepted",
                )
                assert_true(client.json("/api/sessions")[0] == 403, "foreign host accepted")
        for origin in (
            "http://example.com",
            "http://127.0.0.1",
            "http://100.63.255.255",
            "http://100.128.0.0",
            "http://100.64.0.9@evil.example",
            "http://100.64.0.9/path",
            "http://100.64.0.9:99999",
        ):
            rejected = run(root, env, "--web", "--web-origin", origin, binary=binary)
            assert_true(rejected.returncode != 0 and "origin" in rejected.stderr, rejected.stderr)


def test_web_singleton_auth_persistence(root, home, *, binary):
    project = root / "outside-launch"
    project.mkdir()
    (root / ".mcp.json").write_text('{"mcpServers":{"untrusted":{"command":"false"}}}')
    with Server(
        [
            lambda _, _body: event(
                {"content": "A real saved answer", "reasoning_content": "Supplied thinking"}
            )
        ]
    ) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, process, env):
            status, _, _ = client.json("/api/sessions")
            assert_true(status == 401, status)
            status, _, _ = client.json(
                "/api/auth", {"code": code}, headers={"Origin": "https://evil.example"}
            )
            assert_true(status == 403, status)
            client.pair(code)
            assert_true(client.json("/api/auth", {"code": code})[0] == 403, "pairing code reused")
            headers = client.json("/api/sessions")[2]
            assert_true(headers["Cache-Control"] == "no-store", headers)
            assert_true(
                client.json("/api/sessions", headers={"Host": "evil.example"})[0] == 403,
                "host bypass",
            )
            reused = run(project, env, "--web", "--web-port", "12345", binary=binary)
            assert_true(
                reused.returncode == 0 and client.origin in reused.stdout,
                reused.stdout + reused.stderr,
            )
            assert_true(process.poll() is None, "master replaced")
            session = client.create(project)
            peer = run(project, env, "-p", "Independent CLI session", binary=binary)
            assert_true(peer.returncode == 0 and "A real saved answer" in peer.stdout, peer.stderr)
            assert_true(len(provider.requests) == 1, provider.requests)
            client.command("submit", session, text="Inspect this session")
            request_id = f"{client.sequence:032x}"
            payload = dict(
                v=1,
                kind="submit",
                request_id=request_id,
                session_id=session["id"],
                generation=session["generation"],
                text="Inspect this session",
            )
            assert_true(
                client.json("/api/command", payload)[0] == 200, "duplicate not acknowledged"
            )
            value = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and bool(value.get("state", {}).get("view", {}).get("blocks"))
                ),
            )
            assert_true(len(provider.requests) == 2, provider.requests)
            files = list((home / ".uagent/history").rglob("*.json"))
            assert_true(len(files) == 1, files)
            data = json.loads(files[0].read_text().splitlines()[1])
            assert_true("display" in data, data.keys())
            assert_true("Supplied thinking" in json.dumps(value), value)
            assert_true(
                all("display" not in message for message in provider.requests[0][1]["messages"]),
                provider.requests,
            )
            assert_true(
                client.json("/api/command", {**payload, "text": "different"})[0] == 409,
                "request ID conflict accepted",
            )
            assert_true(
                client.json(
                    "/api/command", {**payload, "request_id": "f" * 32, "generation": "stale"}
                )[0]
                == 409,
                "stale generation accepted",
            )
            status, denied, _ = client.json(
                "/api/command",
                {
                    "v": 1,
                    "request_id": "d" * 32,
                    "kind": "submit",
                    "session_id": session["id"],
                    "generation": session["generation"],
                    "text": "/context",
                },
            )
            assert_true(status == 409 and "Raw context" in denied["error"], denied)
            status, denied, _ = client.json(
                "/api/command", {**payload, "request_id": "e" * 32, "text": "/fork"}
            )
            assert_true(status == 409 and "conversation controls" in denied["error"], denied)
            assert_true(
                len(list((home / ".uagent/history").rglob("*.json"))) == 1,
                "slash fork changed worker identity",
            )
            client.command("close", session)
            assert_true(
                "Supplied thinking" in json.dumps(client.snapshot(session)),
                "thinking lost on reopen",
            )
            catalogue = client.json("/api/sessions?refresh=1")[1]
            assert_true(
                any(item["title"] == "Inspect this session" for item in catalogue["sessions"]),
                catalogue,
            )
            client.command("logout")
            assert_true(client.json("/api/sessions")[0] == 401, "revoked cookie remained valid")


def test_web_atomic_images_and_session_isolation(root, home, *, binary):
    project = root / "images"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "Image received"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            other = client.create(project)
            client.until(other, lambda value: value["metadata"]["status"] == "idle")
            endpoint = f"/api/sessions/{session['id']}/attachments"
            status, generic, _ = client.json(
                endpoint,
                raw=b"<svg onload='alert(1)'></svg>",
                headers={"Content-Type": "image/png"},
            )
            assert_true(status == 200 and not generic["image"], generic)
            status, body, headers = client.request(
                f"/api/sessions/{session['id']}/assets/{generic['id']}"
            )
            assert_true(
                body == b"<svg onload='alert(1)'></svg>"
                and headers["Content-Type"] == "application/octet-stream",
                headers,
            )
            status, image, _ = client.json(endpoint, raw=PNG, headers={"Content-Type": "image/png"})
            assert_true(status == 200, image)
            client.command("submit", session, text="", attachment_ids=[image["id"]])
            value = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and bool(value["state"].get("view", {}).get("blocks"))
                ),
            )
            assert_true(image["id"] in json.dumps(value), value)
            assert_true("base64," not in json.dumps(value), value)
            client.command("close", session)
            assert_true(image["id"] in json.dumps(client.snapshot(session)), "image reference lost")
            asset = f"/api/sessions/{session['id']}/assets/{image['id']}"
            assert_true(client.request(asset)[1] == PNG, "source asset changed")
            anonymous = WebClient(client.port)
            assert_true(anonymous.request(asset)[0] == 401, "private image was public")
            outside = root / "separate-workspace"
            outside.mkdir()
            peer = client.create(outside)
            status, denied, _ = client.json(
                "/api/command",
                {
                    "v": 1,
                    "request_id": "b" * 32,
                    "kind": "submit",
                    "session_id": peer["id"],
                    "generation": peer["generation"],
                    "attachment_ids": [image["id"]],
                    "text": "Wrong session image",
                },
            )
            assert_true(status == 409 and "unavailable" in denied["error"], denied)
            record = json.loads((home / f".uagent/web/drafts/{session['id']}.json").read_text())
            source = Path(record["path"] + f".assets/{image['id']}.data")
            source.unlink()
            source.symlink_to(home / ".uagent/web/devices.json")
            assert_true(client.request(asset)[0] == 404, "asset read followed a symlink")


def test_web_approval_interrupt_and_independent_workers(root, home, *, binary):
    first = root / "first"
    second = root / "second"
    first.mkdir()
    second.mkdir()

    continued = threading.Event()
    release = threading.Event()

    def responder(_, body):
        messages = body["messages"]
        content = json.dumps(messages)
        if "CONTINUE_AFTER_INTERRUPT" in content:
            continued.set()
            release.wait(timeout=budget(10))
            return event({"content": "continued successfully"})
        if "WEB_SECOND_WORKSPACE" in content:
            return event({"content": "other workspace works"})
        if any(message.get("role") == "tool" for message in messages):
            return event({"content": "done"})
        return tool_call("run", {"command": "sleep 30"}, call_id="long-run")

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(first)
            parallel = client.create(first)
            client.command("submit", session, text="Run the test command")
            value = client.until(session, lambda value: bool(value.get("pending")))
            assert_true(value["pending"]["approval"]["preview"], value)
            client.command("permissions", session, mode="yolo")
            assert_true(
                client.snapshot(session)["pending"]["id"] == value["pending"]["id"],
                "permission change answered pending approval",
            )
            client.command("permissions", session, mode="ask")
            client.command("submit", parallel, text="WEB_SECOND_WORKSPACE")
            client.until(parallel, lambda value: "other workspace works" in json.dumps(value))
            decision = value["pending"]["id"]
            client.command("reply", session, interaction_id=decision, text="y")
            wait_until(
                lambda: not client.snapshot(session)["pending"],
                "approval did not resolve",
                timeout=budget(5),
            )
            started = time.monotonic()
            client.command("interrupt", session)
            ended = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            assert_true(
                time.monotonic() - started < budget(8), "interrupt waited for the tool timeout"
            )
            assert_true("interrupted" in json.dumps(ended), ended)
            assert_true(
                client.snapshot(parallel)["metadata"]["status"] == "idle",
                "another worker was stopped",
            )
            client.command("submit", session, text="CONTINUE_AFTER_INTERRUPT")
            try:
                assert_true(
                    continued.wait(timeout=budget(5)), "continuation never reached the model"
                )
                # Provider receipt and worker IPC are independent observations;
                # wait for the corresponding phase event to reach the master.
                active = client.until(
                    session, lambda value: value["state"].get("activity") == "Waiting for model"
                )
                assert_true(active["metadata"]["turn_active"], active)
                assert_true(not active["state"].get("error"), active["state"].get("error"))
                assert_true(active["state"]["activity"] == "Waiting for model", active["state"])
                assert_true(not active["state"].get("stop"), active["state"].get("stop"))
            finally:
                release.set()
            client.until(
                session,
                lambda value: (
                    "continued successfully" in json.dumps(value)
                    and not value["metadata"]["turn_active"]
                ),
            )
            snapshots = list((home / ".uagent/history").rglob("web-*.json"))
            assert_true(len(snapshots) == 2, snapshots)


def test_web_restart_durable_images_and_read_only_catalogue(root, home, *, binary):
    project = root / "durable-images"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "Durable image answer"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            saved = client.create(project)
            status, asset, _ = client.json(
                f"/api/sessions/{saved['id']}/attachments",
                raw=PNG,
                headers={"Content-Type": "image/png"},
            )
            assert_true(status == 200, asset)
            client.command("submit", saved, text="Keep this original", attachment_ids=[asset["id"]])
            client.until(
                saved,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and "Durable image answer" in json.dumps(value)
                ),
            )
            draft = client.create(project, activate=False)
            cookie = client.cookie
        with web_host(binary, root, home, provider.url) as (client, _, _, _):
            client.cookie = cookie
            listing = client.json("/api/sessions")[1]
            assert_true(
                {saved["id"], draft["id"]} <= {item["id"] for item in listing["sessions"]}, listing
            )
            assert_true(
                all(not item["generation"] for item in listing["sessions"]),
                "opening history created a worker",
            )
            value = client.snapshot(saved)
            assert_true(asset["id"] in json.dumps(value), value)
            assert_true(
                client.request(f"/api/sessions/{saved['id']}/assets/{asset['id']}")[1] == PNG,
                "restart changed original bytes",
            )
            private = client.json(f"/api/sessions/{saved['id']}?detail=m-1")[1]
            assert_true(private["text"] == "", "internal message exposed")
            active = client.command("activate", saved)["session"]
            client.until(active, lambda value: value["metadata"]["status"] == "idle")
            client.command(
                "submit", active, text="Inspect this original again", attachment_ids=[asset["id"]]
            )
            client.until(active, lambda value: value["metadata"]["status"] == "idle")
            assert_true(len(provider.requests) == 2, provider.requests)
            image_parts = [
                part
                for message in provider.requests[-1][1]["messages"]
                if isinstance(message.get("content"), list)
                for part in message["content"]
                if part.get("type") == "image_url"
            ]
            assert_true(len(image_parts) == 1, provider.requests[-1])
            assert_true(
                base64.b64decode(image_parts[0]["image_url"]["url"].split(",", 1)[1]) == PNG,
                "resume did not preserve the original image",
            )


def test_web_concurrent_retry_and_resync(root, home, *, binary):
    project = root / "retry"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "One accepted submission"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            payload = {
                "v": 1,
                "kind": "submit",
                "request_id": "a" * 32,
                "session_id": session["id"],
                "generation": session["generation"],
                "text": "Once only",
            }
            results = []
            gate = threading.Barrier(3)

            def send():
                peer = WebClient(client.port)
                peer.cookie = client.cookie
                gate.wait()
                results.append(peer.json("/api/command", payload)[0])

            threads = [threading.Thread(target=send) for _ in range(2)]
            for thread in threads:
                thread.start()
            gate.wait()
            for thread in threads:
                thread.join(timeout=budget(10))
            assert_true(results == [200, 200], results)
            client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and "One accepted submission" in json.dumps(value)
                ),
            )
            assert_true(len(provider.requests) == 1, provider.requests)
            status, body, _ = client.request("/api/events?cursor=obsolete:999")
            assert_true(status == 200 and b"event: resync" in body, body)


def test_web_project_trust_and_config_precedence(root, home, *, binary):
    first, second = root / "trusted", root / "denied"
    for project in (first, second):
        (project / ".uagent").mkdir(parents=True)
        (project / ".uagent/.config").write_text("UAGENT_MODEL=project/model\n")
    global_config = home / ".uagent/.config"
    global_config.parent.mkdir(exist_ok=True)
    global_config.write_text("UAGENT_MODEL=global/model\n")
    try:
        with Server([lambda _, _body: event({"content": "Configured"})]) as provider:
            with web_host(binary, root, home, provider.url, extra_env={"UAGENT_MODEL": None}) as (
                client,
                code,
                _,
                _,
            ):
                client.pair(code)
                for project, decision, expected in (
                    (first, "y", "project/model"),
                    (second, "n", "global/model"),
                ):
                    session = client.create(project)
                    value = client.until(session, lambda value: bool(value.get("pending")))
                    assert_true(value["pending"]["kind"] == "project.trust", value)
                    assert_true(not provider.requests, "trust triggered a model call")
                    client.command(
                        "reply", session, interaction_id=value["pending"]["id"], text=decision
                    )
                    value = client.until(
                        session, lambda value: value["metadata"]["status"] == "idle"
                    )
                    assert_true(value["state"]["route"] == expected, value)
    finally:
        global_config.unlink(missing_ok=True)


def test_web_worker_cap_and_crash_isolation(root, home, *, binary):
    with Server([lambda _, _body: event({"content": "Unaffected worker"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, process, _):
            client.pair(code)
            sessions = []
            for index in range(5):
                project = root / f"workspace-{index}"
                project.mkdir()
                sessions.append(client.create(project, activate=index < 4))
            status, result, _ = client.json(
                "/api/command",
                {
                    "v": 1,
                    "request_id": "e" * 32,
                    "kind": "activate",
                    "session_id": sessions[-1]["id"],
                },
            )
            assert_true(status == 409 and "limit" in result["error"], result)
            rows = subprocess.check_output(["ps", "-ax", "-o", "pid=,ppid=,args="], text=True)
            victim = next(
                int(row.split()[0])
                for row in rows.splitlines()
                if row.split()[1] == str(process.pid) and str(root / "workspace-0") in row
            )
            os.kill(victim, signal.SIGKILL)
            client.until(sessions[0], lambda value: value["metadata"]["status"] == "interrupted")
            client.command("submit", sessions[1], text="Continue independently")
            client.until(sessions[1], lambda value: "Unaffected worker" in json.dumps(value))
            client.command("close", sessions[0])
            resumed = client.command("activate", sessions[0])["session"]
            assert_true(resumed["generation"] != sessions[0]["generation"], resumed)
            client.until(resumed, lambda value: value["metadata"]["status"] == "idle")


def test_web_slow_upload_does_not_block_controls(root, home, *, binary):
    project = root / "slow-upload"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "Image accepted"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            endpoint = f"/api/sessions/{session['id']}/attachments"
            status, image, _ = client.json(endpoint, raw=PNG, headers={"Content-Type": "image/png"})
            assert_true(status == 200, image)
            with socket.create_connection(("127.0.0.1", client.port)) as slow:
                headers = (
                    f"POST {endpoint} HTTP/1.1\r\nHost: 127.0.0.1:{client.port}\r\n"
                    f"Origin: {client.origin}\r\nCookie: {client.cookie}\r\n"
                    "Content-Type: image/png\r\nContent-Length: 1000\r\n\r\n"
                )
                slow.sendall(headers.encode() + PNG[:16])
                time.sleep(0.1)
                result = []
                thread = threading.Thread(
                    target=lambda: result.append(
                        client.json(
                            "/api/command",
                            {
                                "v": 1,
                                "request_id": "c" * 32,
                                "kind": "submit",
                                "session_id": session["id"],
                                "generation": session["generation"],
                                "text": "Image after upload",
                                "attachment_ids": [image["id"]],
                            },
                        )
                    )
                )
                thread.start()
                try:
                    time.sleep(0.1)
                    started = time.monotonic()
                    client.command("interrupt", session)
                    assert_true(time.monotonic() - started < budget(1.5), "upload blocked Stop")
                finally:
                    slow.shutdown(socket.SHUT_RDWR)
                    thread.join(timeout=budget(10))
                assert_true(result and result[0][0] == 200, result)


def test_web_save_failure_remains_visible(root, home, *, binary):
    project = root / "save-failure"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "Still in memory"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            record = json.loads((home / f".uagent/web/drafts/{session['id']}.json").read_text())
            Path(record["path"]).mkdir()
            client.command("submit", session, text="Exercise a failed checkpoint")
            value = client.until(
                session, lambda value: "cannot save session" in value["state"].get("error", "")
            )
            assert_true("Still in memory" in json.dumps(value), value)
            assert_true(len(provider.requests) == 1, provider.requests)


def test_web_conversation_management_and_statistics(root, home, *, binary):
    import fcntl

    project = root / "shared-project"
    project.mkdir()
    sentinel = project / "keep.txt"
    sentinel.write_text("project files survive")
    usage = {
        "prompt_tokens": 120,
        "completion_tokens": 40,
        "completion_tokens_details": {"reasoning_tokens": 10},
        "prompt_tokens_details": {"cached_tokens": 20},
    }
    with Server([lambda _, _body: event({"content": "Recorded answer"}, usage=usage)]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            draft = client.create(project, activate=False)
            client.command("rename", draft, title="Before activation")
            assert_true(client.snapshot(draft)["metadata"]["title"] == "Before activation", draft)
            session = client.command("activate", draft)["session"]
            client.until(session, lambda value: value["metadata"]["status"] == "idle")
            client.command("rename", session, title="test")
            client.until(session, lambda value: value["metadata"]["title"] == "test")
            client.command("close", session)
            saved = client.snapshot(session)["metadata"]
            assert_true(saved["title"] == "test", saved)
            session = client.command("activate", saved)["session"]
            client.until(session, lambda value: value["metadata"]["status"] == "idle")
            endpoint = f"/api/sessions/{session['id']}/attachments"
            status, image, _ = client.json(endpoint, raw=PNG, headers={"Content-Type": "image/png"})
            assert_true(status == 200, image)
            client.command(
                "submit",
                session,
                text="This must not replace my custom title",
                attachment_ids=[image["id"]],
            )
            value = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle" and value["state"].get("turns") == 1
                ),
            )
            assert_true(value["metadata"]["title"] == "test", value["metadata"])
            facts = value["state"]["view"]["blocks"]
            assert_true(all(block.get("time") for block in facts), facts)
            answer = next(block for block in facts if block["kind"] == "assistant")
            assert_true(answer["route"] and answer["ttft_ms"] >= 0, answer)
            assert_true(
                answer["usage_reported"]
                and answer["usage"]["output"] == 30
                and answer["usage"]["reasoning"] == 10,
                answer,
            )
            assert_true(answer["tokens_per_second"] > 0, answer)
            stats = value["state"]["statistics"]
            assert_true(
                stats["complete"]
                and stats["model_calls"] == 1
                and stats["recorded_turns"] == 1
                and stats["ttft_samples"] == 1,
                stats,
            )
            # Changing the next route never relabels the saved response.
            client.command("submit", session, text="/effort low")
            client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle" and value["state"].get("effort") == "low"
                ),
            )
            assert_true(
                next(
                    block
                    for block in client.snapshot(session)["state"]["view"]["blocks"]
                    if block["kind"] == "assistant"
                )
                == answer,
                "historical model changed",
            )

            def rejected(kind, target, **fields):
                client.sequence += 1
                return client.json(
                    "/api/command",
                    {
                        "v": 1,
                        "request_id": f"{client.sequence:032x}",
                        "kind": kind,
                        "session_id": target["id"],
                        "generation": target.get("generation", ""),
                        **fields,
                    },
                )

            assert_true(rejected("delete", session)[0] == 409, "deleted a live worker")
            client.command("close", session)
            saved = client.snapshot(session)["metadata"]
            assert_true(
                client.snapshot(saved)["state"]["statistics"] == stats,
                "statistics changed on reopen",
            )
            assert_true(
                rejected("rename", saved, title="bad\nname")[0] == 409, "accepted control character"
            )
            marker = home / f".uagent/web/drafts/{session['id']}.json"
            path = Path(json.loads(marker.read_text())["path"])
            # The conversation writer lease still excludes a second writer.
            with Path(str(path) + ".lock").open("r+") as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                assert_true(
                    rejected("rename", saved, title="locked")[0] == 409, "writer lease bypassed"
                )
                assert_true(
                    rejected("delete", saved)[0] == 409, "deleted another writer's conversation"
                )
                assert_true(path.exists(), "locked history removed")
            client.command("rename", saved, title="Saved name")
            renamed = client.snapshot(saved)
            assert_true(renamed["metadata"]["title"] == "Saved name", renamed["metadata"])
            assert_true(renamed["state"]["statistics"] == stats, renamed["state"])
        # Restart the master: the title, metrics, and source image all survive.
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            saved = client.snapshot(saved)["metadata"]
            assert_true(saved["title"] == "Saved name", saved)
            assert_true(
                client.snapshot(saved)["state"]["statistics"] == stats, "restart lost stats"
            )
            client.command("delete", saved)
            assert_true(
                client.json(f"/api/sessions/{saved['id']}")[0] == 404,
                "deleted history remained visible",
            )
            assert_true(
                not path.exists()
                and not Path(str(path) + ".assets").exists()
                and not Path(str(path) + ".events.jsonl").exists()
                and not marker.exists(),
                "owned sidecars remained",
            )
            assert_true(
                sentinel.read_text() == "project files survive", "project data was modified"
            )
            assert_true(
                not client.json("/api/sessions?refresh=1")[1]["sessions"],
                "deleted session resurrected",
            )
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            assert_true(
                not client.json("/api/sessions?refresh=1")[1]["sessions"],
                "deleted draft resurrected on restart",
            )


def test_web_immediate_message_model_control_and_receipts(root, home, *, binary):
    project = root / "message-controls"
    project.mkdir()
    started = threading.Event()
    release = threading.Event()

    def answer(_, _body):
        started.set()
        assert release.wait(timeout=budget(10)), "test did not release provider"
        return event({"content": "Confirmed response"})

    efforts = ["low", "medium", "high", "xhigh", "max"]
    catalog_started = threading.Event()
    release_catalog = threading.Event()

    def catalog_response(_handler):
        catalog_started.set()
        assert release_catalog.wait(timeout=budget(10)), "test did not release catalog"
        return {"data": [{"id": "test", "supported_reasoning_efforts": efforts}]}

    with Server([answer], get_response=catalog_response) as provider:
        providers = {
            "local": {
                "base_url": provider.url,
                "context": 16384,
                "models": {"main": {"id": "test"}},
            }
        }
        with web_host(
            binary,
            root,
            home,
            provider.url,
            extra_env={"UAGENT_PROVIDERS": json.dumps(providers), "UAGENT_MODEL": "local/main"},
        ) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            catalogs = []
            reader = threading.Thread(
                target=lambda: catalogs.append(
                    client.command("model", session, operation="catalog")
                )
            )
            reader.start()
            try:
                assert catalog_started.wait(timeout=budget(5))
                snapshot = client.snapshot(session)
                assert_true(
                    not snapshot["metadata"]["turn_active"] and not snapshot["pending"], snapshot
                )
                # One input can wait behind a control without becoming guidance.
                client.command("submit", session, text="Visible before provider responds")
                request_id = f"{client.sequence:032x}"
                assert_true(not provider.requests, "input ran before the model control finished")
            finally:
                release_catalog.set()
                reader.join(timeout=budget(10))
            assert_true(not reader.is_alive() and catalogs, "catalog receipt did not complete")
            catalog = catalogs[0]
            models = catalog.get("result", {}).get("models", [])
            assert_true(len(models) == 1 and models[0]["value"] == "local/main", catalog)
            assert_true(models[0]["efforts"] == efforts, catalog)
            try:
                assert started.wait(timeout=budget(5))
                snapshot = client.snapshot(session)
                assert_true(snapshot["state"]["efforts"] == ["default", *efforts], snapshot)
                rows = snapshot["state"]["view"]["blocks"]
                assert_true(len(rows) == 1 and rows[0]["kind"] == "user", rows)
                assert_true(rows[0]["request_id"] == request_id, rows)
                saved = list((home / ".uagent/history").rglob("*.json"))
                assert_true(len(saved) == 1 and "Visible before" in saved[0].read_text(), saved)
                receipt = client.json(f"/api/receipts/{request_id}")[1]
                assert_true(receipt["accepted"] and not receipt.get("pending"), receipt)
            finally:
                release.set()
            final = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            assert_true(final["metadata"]["incoming"] == 1, final)
            client.command("close", session)
            catalog = client.json("/api/sessions?refresh=1")[1]
            assert_true(catalog["sessions"][0]["incoming"] == 1, catalog)
            assert_true(len(provider.requests) == 1, "control or inspection invoked the model")


def test_web_background_inspection_and_full_exchange(root, home, *, binary):
    project = root / "background-inspection"
    project.mkdir()
    content = "α🙂" * 9000 + "FULL_BODY_END"
    (project / "large.txt").write_text(content)

    def answer(_, body):
        results = [message for message in body["messages"] if message.get("role") == "tool"]
        if not results:
            return tool_call(
                "run",
                {"command": "sleep 1; printf BACKGROUND_DONE", "yield_ms": 250},
                call_id="background-run",
            )
        if len(results) == 1:
            return tool_call("read_path", {"path": "large.txt"}, call_id="full-read")
        return event({"content": "Foreground done"})

    with Server([answer]) as provider:
        with web_host(
            binary, root, home, provider.url, extra_env={"UAGENT_READ_FILE_BYTES": "100000"}
        ) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("submit", session, text="/yolo")
            client.until(session, lambda value: value["state"].get("yolo", False))
            client.command("submit", session, text="Run background and read the complete file")
            snapshot = client.until(
                session,
                lambda value: any(
                    row.get("status") == "completed" for row in value["state"].get("activities", [])
                ),
            )
            row = snapshot["state"]["activities"][0]
            first = client.command("activity", session, operation="inspect", activity_id=row["id"])[
                "result"
            ]
            second = client.command(
                "activity", session, operation="inspect", activity_id=row["id"]
            )["result"]
            assert_true(first == second and "BACKGROUND_DONE" in first["output"], (first, second))
            snapshot = client.until(
                session,
                lambda value: any(
                    block["kind"] == "activity" for block in value["state"]["view"]["blocks"]
                ),
            )
            blocks = snapshot["state"]["view"]["blocks"]
            assert_true(
                len([block for block in blocks if block["kind"] == "activity"]) == 1, blocks
            )
            raw = ""
            offset = 0
            while True:
                status, page, _ = client.json(
                    f"/api/sessions/{session['id']}?detail=t-full-read&raw=1&offset={offset}"
                )
                assert_true(status == 200, page)
                raw += page["text"]
                if not page["more"]:
                    break
                assert_true(page["next"] > offset, page)
                offset = page["next"]
            exchange = json.loads(raw)
            assert_true(exchange["complete"] and "FULL_BODY_END" in exchange["response"], exchange)
            assert_true("�" not in exchange["response"], "UTF-8 page boundary split")
            count = len(provider.requests)
            client.command("activity", session, operation="list")
            assert_true(len(provider.requests) == count, "inspection invoked model")


def test_web_child_controls_and_conversation_ownership(root, home, *, binary):
    project = root / "child-controls"
    project.mkdir()
    child_started = threading.Event()
    release_child = threading.Event()

    def answer(_, body):
        users = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "user"
        ]
        if "WEB_CHILD_SEED" in users:
            if "WEB_CHILD_FOLLOWUP" in users:
                assert any(message.get("content") == "Child result" for message in body["messages"])
                return event({"content": "Child follow-up result"})
            child_started.set()
            assert release_child.wait(timeout=budget(10))
            return event({"content": "Child result"})
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "Parent ready"})
        return tool_call(
            "subagent", {"prompt": "WEB_CHILD_SEED", "background": True}, call_id="child-spawn"
        )

    with Server([answer]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            peer = client.create(project)
            client.command("submit", session, text="/yolo")
            client.until(session, lambda value: value["state"].get("yolo", False))
            client.command("submit", session, text="Delegate a child")
            try:
                assert child_started.wait(timeout=budget(5))
                snapshot = client.until(
                    session, lambda value: bool(value["state"].get("activities"))
                )
                child = snapshot["state"]["activities"][0]
                count = len(provider.requests)
                client.command("activity", session, operation="inspect", activity_id=child["id"])
                assert_true(len(provider.requests) == count, "inspection called the model")
                denied = client.json(
                    "/api/command",
                    {
                        "v": 1,
                        "request_id": "e" * 32,
                        "kind": "activity",
                        "operation": "inspect",
                        "session_id": peer["id"],
                        "generation": peer["generation"],
                        "agent_id": child["agent_id"],
                    },
                )
                assert_true(
                    denied[0] == 409 and "another conversation" in denied[1]["error"], denied[1]
                )
            finally:
                release_child.set()
            snapshot = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and any(
                        row.get("status") == "completed"
                        for row in value["state"].get("activities", [])
                    )
                ),
            )
            detail = client.command(
                "activity", session, operation="inspect", activity_id=child["id"]
            )["result"]
            assert_true("Child result" in json.dumps(detail["conversation"]), detail)
            client.command(
                "activity",
                session,
                operation="followup",
                agent_id=child["agent_id"],
                text="WEB_CHILD_FOLLOWUP",
            )
            client.until(
                session,
                lambda value: (
                    len(value["state"].get("activities", [])) >= 2
                    and all(row["status"] == "completed" for row in value["state"]["activities"])
                ),
            )
            detail = client.command(
                "activity", session, operation="inspect", agent_id=child["agent_id"]
            )["result"]
            assert_true("Child follow-up result" in json.dumps(detail["conversation"]), detail)


def test_web_http_context_configuration_permissions_and_fork(root, home, *, binary):
    project = root / "context-fork"
    project.mkdir()
    answer = "Exact raw answer α🙂"
    with Server([lambda _, _body: event({"content": answer})]) as provider:
        with web_host(
            binary, root, home, provider.url, extra_env={"UAGENT_API_KEY": "private-http-test-key"}
        ) as (client, code, _, _):
            client.pair(code)
            config = client.command("config")["result"]
            settings = {item["name"]: item for item in config["settings"]}
            assert_true(len(settings) > 80 and settings["UAGENT_API_KEY"]["value"] is None, config)
            changed = client.command(
                "config",
                operation="apply",
                scope="user",
                changes=[{"key": "UAGENT_APPROVAL", "value": "yolo"}],
            )["result"]
            assert_true(
                changed["effects"][0]["effect"] == "active at the next user turn",
                changed["effects"],
            )
            session = client.create(project)
            initial = client.snapshot(session)
            assert_true(
                initial["state"]["permissions"]
                == {"mode": "default", "effective": "yolo", "default": "yolo"},
                initial,
            )
            client.command("permissions", session, mode="ask")
            preview = client.command("context", session)["result"]["exchanges"][0]
            assert_true(preview["preview"] and not provider.requests, preview)

            def body_for(target, exchange, part):
                text, offset, metadata = "", 0, None
                while True:
                    status, page, _ = client.json(
                        f"/api/sessions/{target['id']}?http={exchange['id']}&part={part}&offset={offset}"
                    )
                    assert_true(status == 200, page)
                    text += page["text"]
                    metadata = page["exchange"]
                    if not page["more"]:
                        return text, metadata
                    assert_true(page["next"] > offset, page)
                    offset = page["next"]

            prepared, _ = body_for(session, preview, "request")
            assert_true("tools" in json.loads(prepared), prepared[:100])
            data = b"\x00\xffarbitrary binary\x00"
            status, asset, _ = client.json(
                f"/api/sessions/{session['id']}/attachments?name=sample.unknown",
                raw=data,
                headers={"Content-Type": "application/octet-stream"},
            )
            assert_true(
                status == 200 and asset["name"] == "sample.unknown" and not asset["image"], asset
            )
            client.command(
                "submit",
                session,
                text="Read attached bytes " + "α🙂" * 3500,
                attachment_ids=[asset["id"]],
            )
            snapshot = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            rows = snapshot["state"]["view"]["blocks"]
            request, reply = rows[0], rows[-1]
            assert_true(reply["reply_to"] == request["id"] == reply["turn_root"], rows)
            exchange = reply["http"][-1]
            captured, metadata = body_for(session, exchange, "request")
            assert_true(
                json.loads(captured) == provider.requests[-1][1],
                "HTTP request differs from provider payload",
            )
            assert_true(
                "private-http-test-key" not in json.dumps(metadata)
                and "[redacted]" in metadata["request_headers"],
                metadata,
            )
            response, _ = body_for(session, exchange, "response")
            assert_true(
                response.startswith("data: ") and response.endswith("data: [DONE]\n\n"), response
            )
            assert_true("tools" in json.loads(captured), "tools absent from HTTP body")
            fork = client.command("fork", session, title="Independent fork")["result"]
            assert_true(fork["id"] != session["id"], fork)
            fork_saved = client.snapshot(fork)
            assert_true(fork_saved["metadata"]["status"] == "saved", fork_saved)
            assert_true(
                body_for(fork, exchange, "response")[0] == response, "fork lost HTTP capture"
            )
            client.command("close", session)
            session = client.snapshot(session)["metadata"]
            client.command("delete", session)
            assert_true(
                client.request(f"/api/sessions/{fork['id']}/assets/{asset['id']}")[1] == data,
                "deleting source removed fork attachment",
            )
            assert_true(
                body_for(fork, exchange, "response")[0] == response,
                "deleting source removed fork HTTP capture",
            )
            active = client.command("activate", fork)["session"]
            current = client.until(active, lambda value: value["metadata"]["status"] == "idle")
            assert_true(current["state"]["permissions"]["mode"] == "ask", current)
            client.command("submit", active, text="Continue the fork")
            client.until(active, lambda value: value["metadata"]["status"] == "idle")
            assert_true(
                answer in json.dumps(provider.requests[-1][1], ensure_ascii=False),
                "fork lost prior conversation",
            )
            assert_true(
                fork["path"] + ".assets/" in json.dumps(provider.requests[-1][1]),
                "fork references deleted source attachment",
            )
            assert_true(
                len(provider.requests) == 2, "inspection or configuration invoked the model"
            )


def test_web_http_retry_and_failed_request_persistence(root, home, *, binary):
    import http.client

    def unavailable(handler, _body):
        write_json_response(handler, {"error": {"message": "temporary unavailable"}}, status=503)

    def rejected(handler, _body):
        write_json_response(handler, {"error": {"message": "terminal rejection"}}, status=400)

    with Server([unavailable, event({"content": "Recovered after retry"}), rejected]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            client.command("submit", session, text="Retry this request")
            snapshot = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            exchanges = snapshot["state"]["view"]["blocks"][-1]["http"]
            assert_true([item["status"] for item in exchanges] == [503, 200], exchanges)
            first = client.json(
                f"/api/sessions/{session['id']}?http={exchanges[0]['id']}&part=response"
            )[1]
            assert_true(
                json.loads(first["text"])["error"]["message"] == "temporary unavailable", first
            )
            listing = client.json("/api/sessions")[1]
            client.command("submit", session, text="Fail this request")
            stream = http.client.HTTPConnection("127.0.0.1", client.port, timeout=15)
            try:
                stream.request(
                    "GET",
                    f"/api/events?cursor={listing['epoch']}:{listing['cursor']}",
                    headers={"Cookie": client.cookie},
                )
                response = stream.getresponse()
                assert_true(response.status == 200, response.status)
                # Replay every transition: polling can miss a premature idle state.
                while line := response.readline():
                    if not line.startswith(b"data: "):
                        continue
                    frame = json.loads(line[6:])
                    if frame.get("session_id") != session["id"]:
                        continue
                    if frame.get("kind") == "state" and not frame["busy"]:
                        assert_true(frame["checkpoint"], "idle preceded the final checkpoint")
                        break
                else:
                    raise AssertionError("worker stream ended before the final checkpoint")
            finally:
                stream.close()
            snapshot = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            exchange = snapshot["state"]["http"][-1]
            failed_user = next(
                block
                for block in snapshot["state"]["view"]["blocks"]
                if block["text"] == "Fail this request"
            )
            assert_true(
                failed_user["http"][-1]["id"] == exchange["id"],
                "failed HTTP call has no durable request link",
            )
            assert_true(exchange["status"] == 400 and exchange["state"] == "failed", exchange)
            client.command("close", session)
            saved = client.json(
                f"/api/sessions/{session['id']}?http={exchange['id']}&part=response"
            )[1]
            assert_true(
                json.loads(saved["text"])["error"]["message"] == "terminal rejection", saved
            )
            assert_true(
                Path(saved["exchange"]["response_path"]).stat().st_mode & 0o777 == 0o600,
                "HTTP body is not private",
            )
            assert_true(len(provider.requests) == 3, provider.requests)


def test_web_presence_tracks_idle_terminal_exit_crash_and_new_history(root, home, *, binary):
    import http.client
    import queue

    from integration_support import run_pty, write_session

    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, env):
            client.pair(code)
            worker = client.create(root)
            assert_true(client.snapshot(worker)["metadata"]["presence"] == "web", worker)
            path = write_session(
                home, "presence", [{"role": "system", "content": "saved"}], cwd=root
            )
            listing = client.json("/api/sessions?refresh=1")[1]
            saved = next(item for item in listing["sessions"] if item["title"] == "presence")
            assert_true(saved["presence"] == "", saved)
            frames = queue.Queue()
            stream = http.client.HTTPConnection("127.0.0.1", client.port, timeout=20)

            def read_events():
                try:
                    stream.request(
                        "GET",
                        f"/api/events?cursor={listing['epoch']}:{listing['cursor']}",
                        headers={"Cookie": client.cookie},
                    )
                    response = stream.getresponse()
                    while line := response.readline():
                        if line.startswith(b"data: "):
                            frames.put(json.loads(line[6:]))
                except (OSError, ValueError):
                    pass

            reader = threading.Thread(target=read_events, daemon=True)
            reader.start()

            def metadata(predicate):
                deadline = time.monotonic() + budget(10)
                while time.monotonic() < deadline:
                    frame = frames.get(timeout=max(0.01, deadline - time.monotonic()))
                    value = frame.get("metadata")
                    if value and predicate(value):
                        return value
                raise AssertionError("presence metadata was not published over SSE")

            def active():
                metadata(
                    lambda value: value["id"] == saved["id"] and value["presence"] == "terminal"
                )

            try:
                # An idle terminal owns only its conversation, despite a web worker
                # already running in this same folder. No model call is needed.
                code, output = run_pty(
                    root, env, [b"/q\r"], args=("-c",), before_payload=active, binary=binary
                )
                assert_true(code == 0, output)
                metadata(lambda value: value["id"] == saved["id"] and value["presence"] == "")
                code, output = run_pty(
                    root,
                    env,
                    [lambda process: process.kill()],
                    args=("-c",),
                    before_payload=active,
                    binary=binary,
                )
                assert_true(code == -signal.SIGKILL, output)
                metadata(lambda value: value["id"] == saved["id"] and value["presence"] == "")
                assert_true(
                    Path(str(path) + ".lock").exists(), "ownership file should survive a crash"
                )
                write_session(
                    home, "new-terminal-history", [{"role": "system", "content": "new"}], cwd=root
                )
                discovered = metadata(lambda value: value["title"] == "new-terminal-history")
                assert_true(discovered["updated"] > 0 and discovered["presence"] == "", discovered)
                client.command("close", worker)
                metadata(lambda value: value["id"] == worker["id"] and value["presence"] == "")
                assert_true(len(provider.requests) == 0, provider.requests)
            finally:
                if stream.sock:
                    stream.sock.shutdown(socket.SHUT_RDWR)
                reader.join(timeout=2)
                stream.close()
