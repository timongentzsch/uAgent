"""Detached remote hosts share ownership and refuse unsafe replacement."""

import concurrent.futures
import http.client
import json
import os
import pathlib
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time

from integration_support import assert_true, base_env, budget, wait_until
from session_support import remove_runtime
from web_support import web_host


def test_remote_disconnect_ignores_failures_from_inflight_streams(root, home, *, binary):
    remote_home = root / "remote-home"
    remote_home.mkdir()
    managed = remote_home / ".uagent/remote"
    managed.mkdir(parents=True)
    (managed / "uagent").symlink_to(binary)
    remote_env = base_env(remote_home, "http://127.0.0.1:1")
    started = subprocess.run(
        [binary, "--remote-start"],
        env=remote_env,
        capture_output=True,
        text=True,
        timeout=budget(20),
    )
    assert_true(started.returncode == 0, started.stderr)
    pid = int((remote_home / ".uagent/web/master.lock").read_text().split()[0])
    tools = root / "bin"
    tools.mkdir()
    ssh = tools / "ssh"
    shutil.copyfile(pathlib.Path(__file__).resolve().parents[1] / "fixtures/ssh_relay.py", ssh)
    ssh.write_text(f"#!{sys.executable}\n" + ssh.read_text())
    ssh.chmod(0o700)
    stream = None
    try:
        with web_host(
            binary,
            root,
            home,
            "http://127.0.0.1:1",
            extra_env={
                "PATH": f"{tools}:{os.environ['PATH']}",
                "UAGENT_TEST_SSH_HOME": str(remote_home),
            },
        ) as (client, code, _, _):
            client.pair(code)
            status, value, _ = client.json("/api/hosts", {"action": "add", "address": "peer"})
            assert_true(status == 200, value)
            host = value["id"]

            def phase():
                return client.json("/api/hosts")[1]["hosts"][0]["phase"]

            wait_until(lambda: phase() == "connected", timeout=budget(10), message="connect failed")
            prefix = "/api/hosts/" + host
            listing = client.json(prefix + "/api/sessions")[1]
            stream = http.client.HTTPConnection("127.0.0.1", client.port, timeout=budget(10))
            stream.request(
                "GET",
                prefix + f"/api/events?cursor={listing['epoch']}:{listing['cursor']}",
                headers={"Cookie": client.cookie},
            )
            response = stream.getresponse()
            assert_true(response.status == 200, response.status)
            assert_true(response.readline() == b"event: ready\n", "stream not ready")
            client.json("/api/hosts", {"action": "disconnect", "id": host})
            wait_until(
                lambda: phase() == "disconnected",
                timeout=budget(5),
                message="explicit disconnect was lost",
            )
            time.sleep(2)
            assert_true(phase() == "disconnected", "stale stream failure reconnected SSH")
            client.json("/api/hosts", {"action": "reconnect", "id": host})
            wait_until(lambda: phase() == "connected", timeout=budget(10), message="retry failed")
            assert_true(
                client.json(prefix + "/api/sessions")[1]["epoch"] == listing["epoch"],
                "disconnect replaced the remote backend",
            )
    finally:
        if stream:
            stream.close()
        os.kill(pid, signal.SIGTERM)
        remove_runtime(remote_home)


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
        status, value, _ = client.json("/api/hosts", {"action": "add", "address": "GPU"})
        assert_true(status == 200, value)
        host = value["id"]

        def failed():
            rows = client.json("/api/hosts")[1]["hosts"]
            return rows and rows[0]["phase"] == "disconnected" and rows[0]["error"]

        wait_until(failed, timeout=budget(10), message="authentication failure did not settle")
        assert_true("ssh-add" in failed(), failed())
        time.sleep(2)
        assert_true(len(attempts.read_text().splitlines()) == 1, "authentication was retried")
        client.json("/api/hosts", {"action": "reconnect", "id": host})
        wait_until(
            lambda: len(attempts.read_text().splitlines()) == 2,
            timeout=budget(10),
            message="manual reconnect did not start a fresh attempt",
        )


def test_remote_start_joins_and_idle_update_preserves_terminals(root, home, *, binary):
    env = base_env(home, "http://127.0.0.1:1")
    env["UAGENT_WEB_TERMINAL"] = "1"

    def start(flag="--remote-start"):
        return subprocess.run(
            [binary, flag], cwd=root, env=env, capture_output=True, text=True, timeout=budget(20)
        )

    pid = None
    cookie = ""
    endpoint = None

    def request(path, body):
        connection = http.client.HTTPConnection("localhost", timeout=budget(10))
        connection.sock = socket.socket(socket.AF_UNIX)
        connection.sock.settimeout(budget(10))
        connection.sock.connect(endpoint["socket"])
        connection.request(
            "POST",
            path,
            json.dumps(body),
            {"Origin": endpoint["origin"], "Cookie": cookie, "Content-Type": "application/json"},
        )
        response = connection.getresponse()
        value = json.loads(response.read())
        headers = dict(response.getheaders())
        connection.close()
        return response.status, value, headers

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda _: start(), range(3)))
        assert_true(
            all(result.returncode == 0 for result in results), [result.stderr for result in results]
        )
        identities = [json.loads(result.stdout) for result in results]
        assert_true(len({value["epoch"] for value in identities}) == 1, identities)
        endpoint = json.loads(start().stdout)
        pid = int((home / ".uagent/web/master.lock").read_text().split()[0])
        assert_true(stat.S_IMODE(os.stat(endpoint["socket"]).st_mode) == 0o600, endpoint)
        grants = [identity["pairing_code"] for identity in [*identities, endpoint]]
        assert_true(len(set(grants)) == len(grants), "discoveries shared a pairing code")
        for grant in grants:
            status, value, headers = request("/api/auth", {"code": grant, "name": "Remote test"})
            assert_true(status == 200, value)
            assert_true(request("/api/auth", {"code": grant})[0] == 403, "grant reused")
        cookie = headers["Set-Cookie"].split(";", 1)[0]
        status, value, _ = request(
            "/api/command",
            {
                "v": 2,
                "request_id": "a" * 32,
                "kind": "terminal",
                "action": "open",
                "cwd": str(root),
                "folder": str(root),
            },
        )
        assert_true(status == 200, value)
        terminal = value["result"]["id"]
        blocked = start("--remote-stop-if-idle")
        assert_true(blocked.returncode != 0 and "active" in blocked.stdout, blocked.stdout)
        # Refusal leaves the same host and terminal alive.
        assert_true(json.loads(start().stdout)["epoch"] == endpoint["epoch"], "host was replaced")
        status, value, _ = request(
            "/api/command",
            {"v": 2, "request_id": "b" * 32, "kind": "terminal", "action": "close", "id": terminal},
        )
        assert_true(status == 200, value)
        deadline = time.monotonic() + budget(10)
        while time.monotonic() < deadline:
            stopped = start("--remote-stop-if-idle")
            if stopped.returncode == 0:
                break
            time.sleep(0.02)
        assert_true(stopped.returncode == 0, stopped.stdout)
        pid = None
    finally:
        if pid:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        remove_runtime(home)


def test_remote_start_reuses_installed_web_host_without_replacing_it(root, home, *, binary):
    with web_host(binary, root, home, "http://127.0.0.1:1") as (client, code, _, _):
        env = base_env(home, "http://127.0.0.1:1")
        result = subprocess.run(
            [binary, "--remote-start"], env=env, capture_output=True, text=True, timeout=budget(10)
        )
        assert_true(result.returncode == 0, result.stdout + result.stderr)
        # Authenticated SSH discovery must not invalidate the browser's code.
        client.pair(code)
        identity = json.loads(result.stdout)
        assert_true(identity["epoch"] == client.json("/api/sessions")[1]["epoch"], identity)
        refused = subprocess.run(
            [binary, "--remote-stop-if-idle"],
            env=env,
            capture_output=True,
            text=True,
            timeout=budget(10),
        )
        assert_true(refused.returncode != 0 and "service" in refused.stdout, refused.stdout)
