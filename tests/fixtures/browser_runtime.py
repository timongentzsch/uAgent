#!/usr/bin/env python3
"""Small Chrome/Xvnc stand-ins for the native profile lifecycle test."""

import json
import os
import signal
import socket
import struct
import sys
from pathlib import Path

name = Path(sys.argv[0]).name
if name == "xauth":
    sys.exit(0)
if name == "Xtigervnc":
    # A display that takes a viewer's handshake and writes down its keys.
    path = sys.argv[sys.argv.index("-rfbunixpath") + 1]
    server = socket.socket(socket.AF_UNIX)
    # By its name inside its folder: a test's path outgrows a socket address.
    os.chdir(Path(path).parent)
    server.bind(Path(path).name)
    server.listen()
    while True:
        viewer, _ = server.accept()
        viewer.sendall(b"RFB 003.008\n")
        viewer.recv(12)
        viewer.sendall(b"\x01\x01")
        viewer.recv(1)
        viewer.sendall(bytes(4))
        viewer.recv(1)
        viewer.sendall(bytes(20) + struct.pack(">I", 4) + b"fake")
        while len(pressed := viewer.recv(8)) == 8:
            with open(path + ".keys", "a") as log:
                log.write(f"{pressed[1]} {struct.unpack('>I', pressed[4:])[0]:x}\n")

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

session = "attached"
measured = 0
pending = b""
while chunk := os.read(3, 4096):
    pending += chunk
    while b"\0" in pending:
        packet, pending = pending.split(b"\0", 1)
        command = json.loads(packet)
        with (profile / "cdp.jsonl").open("a") as log:
            log.write(json.dumps(command) + "\n")
        result = {}
        # The test closes the attached tab by leaving this file.
        if (profile / "close-tab").exists():
            (profile / "close-tab").unlink()
            event = {"method": "Target.detachedFromTarget", "params": {"sessionId": session}}
            os.write(4, json.dumps(event).encode() + b"\0")
            session = "reattached"
        if command.get("sessionId", session) != session:
            error = {"message": "Session with given id not found."}
            os.write(4, json.dumps({"id": command["id"], "error": error}).encode() + b"\0")
            continue
        if command["method"] == "Page.getNavigationHistory":
            result = {
                "currentIndex": 1,
                "entries": [
                    {"id": 1, "url": "chrome://password-manager/passwords"},
                    {"id": 2, "url": "https://example.com/"},
                ],
            }
        elif command["method"] == "Target.getTargets":
            result = {
                "targetInfos": [
                    {"targetId": "tab", "type": "page", "url": "about:blank"},
                    {"targetId": "left-open", "type": "page", "url": "https://example.com/"},
                ]
            }
        elif command["method"] == "Runtime.evaluate":
            result = {"result": {"objectId": "field"}}
        elif command["method"] == "Runtime.callFunctionOn":
            # The field grows with every look once the test says a login is saved.
            if (profile / "saved-login-here").exists():
                measured += 1
            result = {"result": {"value": measured}}
        elif command["method"] == "Target.attachToTarget":
            result = {"sessionId": session}
        os.write(4, json.dumps({"id": command["id"], "result": result}).encode() + b"\0")
