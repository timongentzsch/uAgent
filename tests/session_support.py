"""Local session protocol client used by cross-interface and teardown checks."""

import json
import os
import socket
import time
import uuid
from pathlib import Path

from integration_support import budget, fnv1a64


def runtime_directory(home):
    return Path(f"/tmp/uagent-{os.getuid()}-{fnv1a64(str(home / '.uagent'))}")


class SessionClient:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(budget(10))
        self.socket.connect(str(path))
        self.stream = self.socket.makefile("r")
        self.hello = json.loads(self.stream.readline())
        self.frames = []

    def send(self, kind, **values):
        request = uuid.uuid4().hex
        command = {
            **self.hello,
            "kind": kind,
            "request_id": request,
            "client_request_id": request,
            **values,
        }
        self.socket.sendall((json.dumps(command) + "\n").encode())
        return command

    def until(self, predicate, seconds=15):
        self.socket.settimeout(budget(seconds))
        deadline = time.monotonic() + budget(seconds)
        while time.monotonic() < deadline:
            line = self.stream.readline()
            assert line, self.frames[-5:]
            frame = json.loads(line)
            self.frames.append(frame)
            if predicate(frame):
                return frame
        raise AssertionError(self.frames[-5:])

    def close(self):
        self.stream.close()
        self.socket.close()


def stop_sessions(home, settle=0.0):
    """Close every runtime of `home`. With `settle`, also those a finishing
    session starts meanwhile (a thread waking its coordinator): done once no
    socket has appeared for that many seconds."""
    deadline = time.monotonic() + budget(15)
    quiet_since = time.monotonic()
    while time.monotonic() < deadline:
        sockets = list(runtime_directory(home).glob("*.sock"))
        if not sockets:
            if time.monotonic() - quiet_since >= budget(settle):
                return
            time.sleep(0.05)
            continue
        for path in sockets:
            try:
                client = SessionClient(path)
                client.send("close")
                client.socket.settimeout(budget(5))
                while client.stream.readline():
                    pass
                client.close()
            except ConnectionRefusedError:
                # Left by a runtime that crashed: nothing listens on it.
                path.unlink(missing_ok=True)
            except (OSError, ValueError):
                pass
        quiet_since = time.monotonic()
