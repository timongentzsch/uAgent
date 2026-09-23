#!/usr/bin/env python3
"""Small Chrome/Xvnc stand-ins for the native profile lifecycle test."""

import json
import os
import signal
import sys
from pathlib import Path

name = Path(sys.argv[0]).name
if name == "xauth":
    sys.exit(0)
if name == "Xtigervnc":
    Path(sys.argv[sys.argv.index("-rfbunixpath") + 1]).touch()
    signal.pause()
    sys.exit(0)

profile = Path(next(arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--user-data-dir=")))
controlled = "--remote-debugging-pipe" in sys.argv
with (profile / "launches.jsonl").open("a") as log:
    log.write(json.dumps({"controlled": controlled, "arguments": sys.argv[1:]}) + "\n")


def flush_profile(_signal, _frame):
    (profile / "saved-login").write_text("saved during graceful exit")
    sys.exit(0)


signal.signal(signal.SIGTERM, flush_profile)
if not controlled:
    (profile / "manual-ready").touch()
    signal.pause()
    sys.exit(0)

pending = b""
while chunk := os.read(3, 4096):
    pending += chunk
    while b"\0" in pending:
        packet, pending = pending.split(b"\0", 1)
        command = json.loads(packet)
        result = {}
        if command["method"] == "Target.getTargets":
            result = {"targetInfos": [{"targetId": "tab", "type": "page", "url": "about:blank"}]}
        elif command["method"] == "Target.attachToTarget":
            result = {"sessionId": "attached"}
        os.write(4, json.dumps({"id": command["id"], "result": result}).encode() + b"\0")
