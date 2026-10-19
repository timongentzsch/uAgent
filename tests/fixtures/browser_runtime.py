#!/usr/bin/env python3
"""Small Chrome/Xvnc stand-ins for the native profile lifecycle test."""

import json
import os
import re
import signal
import socket
import struct
import sys
import time
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
    log.write(
        json.dumps(
            {
                "controlled": controlled,
                "arguments": sys.argv[1:],
                "display": os.getenv("DISPLAY"),
                "authority": os.getenv("XAUTHORITY"),
            }
        )
        + "\n"
    )


def flush_profile(_signal, _frame):
    (profile / "saved-login").write_text("saved during graceful exit")
    sys.exit(0)


signal.signal(signal.SIGTERM, flush_profile)
if not controlled:
    (profile / "manual-ready").touch()
    signal.pause()
    sys.exit(0)

# Each attached tab has its own session; closing the tab by hand detaches
# them all, and those attached after are named anew.
prefix = "attached"
attached = set()
measured = 0
pending = b""
# The reply a page's dialog holds back, as Chrome does, until it is answered.
withheld = None


def emit(method, session, **params):
    event = {"method": method, "params": params}
    if session:
        event["sessionId"] = session
    os.write(4, json.dumps(event).encode() + b"\0")


def opens_dialog(marker, opened_by):
    """Whether this command opens the dialog the test asked for in `marker`."""
    global withheld
    if not (profile / marker).exists():
        return False
    kind = (profile / marker).read_text()
    (profile / marker).unlink()
    emit(
        "Page.javascriptDialogOpening", opened_by["sessionId"], type=kind, message=f"Really {kind}?"
    )
    withheld = opened_by["id"]
    return True


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
            for gone in sorted(attached):
                emit("Target.detachedFromTarget", None, sessionId=gone)
            attached.clear()
            prefix = "reattached"
        if "sessionId" in command and command["sessionId"] not in attached:
            error = {"message": "Session with given id not found."}
            os.write(4, json.dumps({"id": command["id"], "error": error}).encode() + b"\0")
            continue
        method = command["method"]
        if method == "Page.navigate" and (profile / "pause-navigation").exists():
            (profile / "navigation-paused").touch()
            deadline = time.monotonic() + 5
            while (profile / "pause-navigation").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
        if command["params"].get("type") == "mouseReleased":
            # A click may open a file chooser, held back, or start a download.
            if (profile / "chooser-on-click").exists():
                emit(
                    "Page.fileChooserOpened",
                    command.get("sessionId"),
                    frameId="tab",
                    mode="selectSingle",
                    backendNodeId=7,
                )
            if (profile / "download-on-click").exists():
                (profile / "download-on-click").unlink()
                emit(
                    "Browser.downloadWillBegin",
                    None,
                    guid="g1",
                    url="https://example.com/report.pdf",
                    suggestedFilename="report.pdf",
                )
                emit("Browser.downloadProgress", None, guid="g1", state="completed")
        # A dialog on press, on release (a click), or while the page is looked at.
        looking = re.search(r',"","\w+"\)$', command["params"].get("expression", ""))
        phase = command["params"].get("type") or ("look" if looking else "-")
        marker = {
            "mousePressed": "dialog-on-press",
            "mouseReleased": "dialog-on-click",
            "look": "dialog-on-look",
        }
        if opens_dialog(marker.get(phase, "-"), command):
            continue
        if method == "Page.navigate" and opens_dialog("dialog-on-navigate", command):
            continue
        if method == "Page.handleJavaScriptDialog":
            os.write(4, json.dumps({"id": command["id"], "result": {}}).encode() + b"\0")
            emit(
                "Page.javascriptDialogClosed",
                command["sessionId"],
                result=command["params"]["accept"],
            )
            os.write(4, json.dumps({"id": withheld, "result": {}}).encode() + b"\0")
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
        elif command["method"] == "Page.getLayoutMetrics":
            result = {"cssVisualViewport": {"clientWidth": 800, "clientHeight": 600}}
        elif command["method"] == "Page.captureScreenshot":
            result = {"data": "c2hvdA=="}
        elif command["method"] == "Runtime.evaluate":
            asked = command["params"]["expression"]
            if "scrollIntoView" in asked:
                # Element 1 is on the page; any other is gone.
                value = {"x": 40, "y": 30} if asked.endswith("[0])") else None
                result = {"result": {"value": value}}
            elif asked.endswith(',"clip","")'):
                read = {"url": "https://example.com/", "title": "Clips", "chars": 9, "matches": 1}
                result = {"result": {"value": read | {"text": "Clip one", "links": ""}}}
            elif looking:
                # Every look is at the same page as the one before.
                looked = {
                    "url": "https://example.com/",
                    "title": "Clips",
                    "chars": 5,
                    "fresh": False,
                }
                looked |= {"text": "Clips", "elements": '[1] text "Search" (empty)'}
                result = {"result": {"value": looked}}
            elif "data-uagent-secret" in asked and "activeElement" in asked:
                # True for a password field, or "frame" for another site's frame.
                focused = profile / "password-focused"
                value = focused.exists() and (focused.read_text() or True)
                result = {"result": {"value": value}}
            elif "performance.timeOrigin" in asked:
                # A page that has finished loading and stays as it is.
                result = {"result": {"value": {"ready": "complete", "doc": "1", "sig": "1"}}}
            else:
                result = {"result": {"objectId": "field"}}
        elif command["method"] == "Runtime.callFunctionOn" and "'file'" in str(
            command["params"].get("functionDeclaration")
        ):
            result = {"result": {"value": (profile / "file-input").exists()}}
        elif command["method"] == "Runtime.callFunctionOn":
            # The field grows with every look once the test says a login is saved.
            if (profile / "saved-login-here").exists():
                measured += 1
            result = {"result": {"value": measured}}
        elif command["method"] == "Target.getTargetInfo":
            result = {"targetInfo": {"targetId": "tab", "url": "https://example.com/"}}
        elif command["method"] == "Target.createTarget":
            result = {"targetId": "new"}
        elif command["method"] == "Target.attachToTarget":
            result = {"sessionId": f"{prefix}-{command['params']['targetId']}"}
            attached.add(result["sessionId"])
        os.write(4, json.dumps({"id": command["id"], "result": result}).encode() + b"\0")
