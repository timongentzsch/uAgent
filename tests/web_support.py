"""Hermetic native web host/client shared by protocol and browser tests."""

import contextlib
import http.client
import json
import signal
import socket
import subprocess

from integration_support import base_env, budget, wait_until


def available_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class WebClient:
    def __init__(self, port, origin=None):
        self.port = port
        self.origin = origin or f"http://127.0.0.1:{port}"
        self.cookie = ""
        self.sequence = 0

    def request(self, path, value=None, *, method=None, headers=None, raw=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        body = raw if raw is not None else json.dumps(value) if value is not None else None
        fields = {"Origin": self.origin, "Content-Type": "application/json", "Cookie": self.cookie}
        fields.update(headers or {})
        connection.request(method or ("POST" if body is not None else "GET"), path, body, fields)
        response = connection.getresponse()
        data = response.read()
        result = (response.status, data, dict(response.getheaders()))
        connection.close()
        return result

    def json(self, path, value=None, **kwargs):
        status, body, headers = self.request(path, value, **kwargs)
        return status, json.loads(body), headers

    def pair(self, code):
        status, value, headers = self.json("/api/auth", {"code": code, "name": "Test device"})
        assert status == 200, value
        self.cookie = headers["Set-Cookie"].split(";", 1)[0]
        return value

    def command(self, kind, session=None, **values):
        self.sequence += 1
        payload = {"v": 1, "kind": kind, "request_id": f"{self.sequence:032x}"}
        if session:
            payload.update(session_id=session["id"], generation=session.get("generation", ""))
        payload.update(values)
        status, result, _ = self.json("/api/command", payload)
        assert status == 200, result
        return result

    def snapshot(self, session):
        status, value, _ = self.json("/api/sessions/" + session["id"])
        assert status == 200, value
        return value

    def until(self, session, predicate):
        value = None

        def ready():
            nonlocal value
            value = self.snapshot(session)
            return predicate(value)

        try:
            wait_until(ready, "worker state timed out", timeout=15)
        except AssertionError:
            raise AssertionError(value) from None
        return value

    def create(self, workspace, activate=True):
        session = self.command("create", cwd=str(workspace))["session"]
        if activate:
            session = self.command("activate", session)["session"]
            self.until(
                session,
                lambda value: value["metadata"]["status"] in ("idle", "waiting", "interrupted"),
            )
        return session


@contextlib.contextmanager
def web_host(binary, root, home, provider, port=None, extra_env=None):
    port = port or available_port()
    env = base_env(home, provider)
    for key, value in (extra_env or {}).items():
        if value is None:
            env.pop(key, None)
        else:
            env[key] = value
    log = root / "web-host.log"
    with log.open("w+") as output:
        process = subprocess.Popen(
            [str(binary), "--web", "--web-port", str(port)],
            cwd=root,
            env=env,
            stdout=output,
            stderr=output,
        )
        try:
            discovery = home / ".uagent/web/discovery.json"

            def started():
                if process.poll() is not None:
                    raise AssertionError(log.read_text())
                return discovery.exists() and "Pairing code" in log.read_text()

            wait_until(started, lambda: log.read_text(), timeout=10)
            code = (
                log.read_text().split("Pairing code (single use, 5 minutes): ")[1].splitlines()[0]
            )
            client = WebClient(port)
            yield client, code, process, env
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=budget(12))
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                raise AssertionError(
                    "web master failed bounded shutdown: " + log.read_text()
                ) from None
