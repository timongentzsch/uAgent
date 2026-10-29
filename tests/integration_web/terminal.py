import base64
import json
import os
import socket
import struct
import time

from integration_support import Server, assert_true, budget, event
from web_support import web_host

TEXT, BINARY, CLOSE = 1, 2, 8


class Socket:
    """A minimal websocket client: the browser's side of a terminal."""

    def __init__(self, client, path, origin=None, cookie=None):
        self.connection = socket.create_connection(("127.0.0.1", client.port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{client.port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            f"Origin: {origin or client.origin}\r\n"
            f"Cookie: {client.cookie if cookie is None else cookie}\r\n\r\n"
        )
        self.connection.sendall(request.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.connection.recv(1)
            if not chunk:
                break
            head += chunk
        self.status = int(head.split(b" ", 2)[1]) if head else 0

    def send(self, opcode, payload):
        mask = os.urandom(4)
        size = len(payload)
        header = bytes([0x80 | opcode])
        if size < 126:
            header += bytes([0x80 | size])
        else:
            header += bytes([0x80 | 126]) + struct.pack("!H", size)
        masked = bytes(byte ^ mask[at % 4] for at, byte in enumerate(payload))
        self.connection.sendall(header + mask + masked)

    def text(self, value):
        self.send(TEXT, json.dumps(value).encode())

    def type(self, text):
        self.send(BINARY, text.encode())

    def exact(self, size):
        data = b""
        while len(data) < size:
            chunk = self.connection.recv(size - len(data))
            if not chunk:
                raise ConnectionError("socket closed")
            data += chunk
        return data

    def frame(self):
        first, second = self.exact(2)
        size = second & 0x7F
        if size == 126:
            size = struct.unpack("!H", self.exact(2))[0]
        elif size == 127:
            size = struct.unpack("!Q", self.exact(8))[0]
        return first & 0x0F, self.exact(size)

    def until(self, marker):
        """The output up to `marker`, and any control message on the way."""
        output, notices = b"", []
        deadline = time.monotonic() + budget(10)
        while marker.encode() not in output:
            assert_true(time.monotonic() < deadline, output)
            opcode, payload = self.frame()
            if opcode == BINARY:
                output += payload
            elif opcode == TEXT:
                notices.append(json.loads(payload))
            elif opcode == CLOSE:
                return output.decode(errors="replace"), notices, payload
        return output.decode(errors="replace"), notices, None

    def close(self):
        self.connection.close()


def open_terminal(client, cwd="", folder=""):
    result = client.command("terminal", action="open", cwd=cwd, folder=folder)
    assert_true(result["accepted"], result)
    return result["result"]["id"]


def listed(client):
    """The folders' shells every device lists, from the catalogue."""
    status, catalogue, _ = client.json("/api/sessions")
    assert_true(status == 200, catalogue)
    return catalogue["terminals"]


def test_web_terminal_runs_a_shell_for_paired_devices(root, home, *, binary):
    project = root / "terminal"
    project.mkdir()
    shell = {"SHELL": "/bin/sh", "OPENROUTER_API_KEY": "planted-terminal-key"}
    with Server([event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url, extra_env=shell) as (client, code, _, _):
            client.pair(code)
            assert_true(listed(client) == [], "a shell before any was opened")
            terminal = open_terminal(client, str(project), str(project))
            shells = listed(client)
            assert_true(
                [(entry["id"], entry["folder"]) for entry in shells] == [(terminal, str(project))],
                shells,
            )
            path = f"/api/terminals/{terminal}"

            # Only a paired device, from the app's own origin, reaches it.
            assert_true(Socket(client, path, cookie="").status == 401, "unpaired socket")
            evil = Socket(client, path, origin="http://evil.example")
            assert_true(evil.status == 403, "foreign origin socket")

            first = Socket(client, path)
            assert_true(first.status == 101, first.status)
            first.text({"type": "resize", "rows": 30, "cols": 90})
            first.type('echo "hello-$((6*7))" "[${OPENROUTER_API_KEY:-none}]"; stty size\n')
            output, _, closed = first.until("30 90")
            assert_true(closed is None and "hello-42 [none]" in output, output)
            assert_true("planted-terminal-key" not in output, output)

            # A reconnect gets what the shell printed, and the old socket
            # learns it was replaced.
            second = Socket(client, path)
            output, _, _ = second.until("hello-42 [none]")
            _, notices, _ = first.until("never printed")
            assert_true({"type": "replaced"} in notices, notices)
            first.close()
            second.type("exit 4\n")
            _, notices, _ = second.until("never printed")
            assert_true({"type": "exit", "code": 4} in notices, notices)
            second.close()
            assert_true(listed(client) == [], "an exited shell is still listed")

            # Folders only; closing ends a shell.
            status, refused, _ = client.json(
                "/api/command",
                {
                    "v": 2,
                    "kind": "terminal",
                    "request_id": "f" * 32,
                    "action": "open",
                    "cwd": str(project / "missing"),
                },
            )
            assert_true(status == 409 and "not a folder" in refused["error"], refused)
            terminal = open_terminal(client, folder=str(project))
            assert_true(client.command("terminal", action="close", id=terminal)["accepted"], "")
            assert_true(listed(client) == [], "a closed shell is still listed")

            # The page's own shell, of no folder, is never listed and ends
            # with its one viewer: no other device takes it up.
            own = open_terminal(client)
            assert_true(listed(client) == [], "the page's own shell is listed")
            viewer = Socket(client, f"/api/terminals/{own}")
            viewer.type("echo own-shell\n")
            viewer.until("own-shell")
            viewer.close()
            again = Socket(client, f"/api/terminals/{own}")
            _, notices, _ = again.until("never printed")
            assert_true({"type": "exit", "code": -1} in notices, notices)
            again.close()


def test_web_terminal_can_be_turned_off(root, home, *, binary):
    with Server([event({"content": "unused"})]) as provider:
        with web_host(
            binary, root, home, provider.url, extra_env={"UAGENT_WEB_TERMINAL": "false"}
        ) as (client, code, _, _):
            client.pair(code)
            assert_true(listed(client) is None, "a list of a terminal turned off")
            status, value, _ = client.json(
                "/api/command",
                {"v": 2, "kind": "terminal", "request_id": "e" * 32, "action": "open"},
            )
            assert_true(status == 409 and "turned off" in value["error"], value)
            refused = Socket(client, "/api/terminals/" + "a" * 32)
            assert_true(refused.status == 101, refused.status)
            opcode, payload = refused.frame()
            assert_true(opcode == CLOSE and struct.unpack("!H", payload[:2])[0] == 1008, payload)
            refused.send(CLOSE, payload)
            refused.close()
