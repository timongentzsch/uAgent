#!/usr/bin/env python3
"""Mock-backed host for browser tests; never calls an external model."""

import argparse
import json
import pathlib
import signal
import tempfile
import threading

from integration_support import Server, event, tool_call, write_sse_sequence
from web_support import web_host

parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=pathlib.Path, required=True)
parser.add_argument("--port", type=int, default=8765)
parser.add_argument("--fixture", type=pathlib.Path, required=True)
args = parser.parse_args()
stop = threading.Event()
signal.signal(signal.SIGINT, lambda *_: stop.set())
signal.signal(signal.SIGTERM, lambda *_: stop.set())


def answer(handler, body):
    assert body.get("model") == "model-b", body.get("model")
    texts = [
        message.get("content", "") for message in body["messages"] if message.get("role") == "user"
    ]
    prompt = str(texts[-1]) if texts else ""
    if "Background activity probe" in prompt and not any(
        message.get("tool_call_id") == "browser-activity" for message in body["messages"]
    ):
        return tool_call(
            "run",
            {"command": "printf BROWSER_ACTIVITY; sleep 10", "yield_ms": 250},
            call_id="browser-activity",
        )
    if "request approval" in prompt and not any(
        message.get("role") == "tool" for message in body["messages"]
    ):
        return tool_call(
            "write_file",
            {"path": "browser-proof.txt", "content": "approved from browser"},
            call_id="browser-write",
        )
    content = "# Verified response\n\nA **streamed** answer with a table.\n\n| Check | Result |\n| --- | --- |\n| Native worker | Ready |\n\nInline $x^2 + y^2 = z^2$ and \\(a+b\\).\n\n$$\\int_0^1 x \\, dx = \\frac{1}{2}$$\n\n```python\nprint('hello')\n```\n\nPrices $5 and $10. `<script>bad()</script>`\n\n![blocked](https://example.com/tracker.png)\n\n[unsafe](javascript:alert(1))\n"
    write_sse_sequence(
        handler,
        [
            event({"reasoning_content": "I checked the supplied evidence."}),
            event(
                {},
                usage={
                    "prompt_tokens": 120,
                    "completion_tokens": 40,
                    "completion_tokens_details": {"reasoning_tokens": 10},
                },
            ),
            *[
                event({"content": content[index : index + 80]}, finish=None)
                for index in range(0, len(content), 80)
            ],
        ],
        delay=0.08,
    )
    return None


with (
    tempfile.TemporaryDirectory(prefix="uagent-browser-") as temporary,
    Server(
        [answer],
        get_response={
            "data": [
                {"id": "model-a", "context_length": 1300000},
                {
                    "id": "model-b",
                    "context_length": 1300000,
                    "reasoning": {"default_effort": "low", "supported_efforts": ["low", "high"]},
                },
            ]
        },
    ) as provider,
):
    root = pathlib.Path(temporary)
    home = root / "home"
    home.mkdir()
    project = root / "project"
    project.mkdir()
    providers = {
        "mock": {
            "base_url": provider.url,
            "api_key": "mock-key",
            "protocol": "openrouter",
            "models": {"main": {"id": "model-a", "effort": "high"}},
        }
    }
    with web_host(
        args.binary.resolve(),
        root,
        home,
        provider.url,
        args.port,
        extra_env={
            "UAGENT_PROVIDERS": json.dumps(providers),
            "UAGENT_MODEL": "mock/main:floor:high",
        },
    ) as (
        client,
        code,
        process,
        _,
    ):
        args.fixture.parent.mkdir(parents=True, exist_ok=True)
        args.fixture.write_text(
            json.dumps(
                {
                    "code": code,
                    "project": str(project),
                    "home": str(home),
                    "pid": process.pid,
                    "origin": client.origin,
                }
            )
        )
        args.fixture.chmod(0o600)
        stop.wait()
