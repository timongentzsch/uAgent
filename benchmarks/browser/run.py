#!/usr/bin/env python3
"""Runs one browser task in a conversation of a running web host.

usage: run.py ORIGIN PROMPT [MODEL]     (on the host; ORIGIN as it was started with)

Pairs a temporary client through `uagent --web`, starts a conversation in
~/uagent-browser-bench, submits PROMPT, prints each tool result and answer,
and logs the client out again.
"""

import http.client
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from web_support import WebClient  # noqa: E402

origin, prompt = sys.argv[1], sys.argv[2]
model = sys.argv[3] if len(sys.argv) > 3 else ""
address = urlparse(origin)

# The test client speaks to 127.0.0.1; the host answers to its own name only.
connect = http.client.HTTPConnection
http.client.HTTPConnection = lambda host, *rest, **named: connect(
    address.hostname if host == "127.0.0.1" else host, *rest, **named
)

started = subprocess.run(
    ["uagent", "--web", "--web-port", str(address.port), "--web-origin", origin],
    capture_output=True,
    text=True,
    timeout=30,
)
code = re.search(r"Pairing code[^:]*: (\S+)", started.stdout + started.stderr).group(1)
client = WebClient(address.port, origin)
client.pair(code)
work = Path.home() / "uagent-browser-bench"
work.mkdir(exist_ok=True)
try:
    session = client.create(work)

    def settle(limit):
        deadline = time.time() + limit
        time.sleep(2)
        while time.time() < deadline:
            value = client.snapshot(session)
            if value["metadata"]["status"] in ("idle", "waiting", "interrupted"):
                return value
            time.sleep(3)
        return client.snapshot(session)

    if model:
        client.command("submit", session, text="/model " + model)
        settle(30)
    began = time.time()
    client.command("submit", session, text=prompt)
    value = settle(540)
    state = value["state"]
    print("status", value["metadata"]["status"], "wall", round(time.time() - began, 1))
    print("usage", json.dumps(state.get("usage")))
    for block in state["view"]["blocks"]:
        kind = block.get("kind")
        if kind == "assistant":
            print("ASSISTANT:", block.get("text", ""))
        elif kind == "tool_result":
            print("  tool", block.get("name") or block.get("title"), block.get("status"))
finally:
    client.command("logout")
