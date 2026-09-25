#!/usr/bin/env python3
"""Mock-backed host for browser tests; never calls an external model."""

import argparse
import base64
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
    texts = [
        message.get("content", "") for message in body["messages"] if message.get("role") == "user"
    ]
    prompt = str(texts[-1]) if texts else ""
    expected_model = "model-a" if prompt == "Retained follow-up" else "model-b"
    assert body.get("model") == expected_model, body.get("model")
    if "Summarize the bounded transcript" in str(body["messages"]):
        return event({"content": "COMPACT-PREVIEW-SUMMARY"})
    if prompt == "Delegate preview task" and not any(
        message.get("tool_call_id") == "preview-child" for message in body["messages"]
    ):
        return tool_call(
            "subagent",
            {
                "prompt": "Review the full task.\n\n"
                + "Detailed instructions. " * 800
                + "TASK-END-MARKER",
                "background": False,
            },
            call_id="preview-child",
        )
    if "Exploration probe" in prompt and not any(
        message.get("tool_call_id") == "explore-a" for message in body["messages"]
    ):
        return event(
            {
                "tool_calls": [
                    {
                        "index": i,
                        "id": f"explore-{name}",
                        "function": {"name": "read_path", "arguments": json.dumps({"path": "."})},
                    }
                    for i, name in enumerate(("a", "b"))
                ]
            },
            finish="tool_calls",
        )
    if "Review the full task." in prompt and not any(
        message.get("tool_call_id") == "review-read" for message in body["messages"]
    ):
        return tool_call(
            "read_path",
            {"path": "."},
            call_id="review-read",
        )
    if "Memory receipt probe" in prompt and not any(
        message.get("tool_call_id") == "memory-receipt" for message in body["messages"]
    ):
        return tool_call(
            "memory",
            {"action": "set", "key": "project/browser-proof", "content": "Browser receipt test."},
            call_id="memory-receipt",
        )
    if "Background activity probe" in prompt and not any(
        message.get("tool_call_id") == "browser-activity" for message in body["messages"]
    ):
        return tool_call(
            "run",
            {"command": "printf BROWSER_ACTIVITY; sleep 10", "yield_ms": 250},
            call_id="browser-activity",
        )
    if "Image probe" in prompt and not any(
        message.get("tool_call_id") == "image-read" for message in body["messages"]
    ):
        return tool_call("read_path", {"path": "shot.png"}, call_id="image-read")
    if "Artifact probe" in prompt:
        done = {message.get("tool_call_id") for message in body["messages"]}
        if "artifact-write" not in done:
            return tool_call(
                "write_file",
                {"path": "report.html", "content": "<h1>ARTIFACT_REPORT</h1>"},
                call_id="artifact-write",
            )
        if "artifact-share" not in done:
            return tool_call("artifact", {"path": "report.html"}, call_id="artifact-share")
    if "request approval" in prompt and not any(
        message.get("role") == "tool" for message in body["messages"]
    ):
        return tool_call(
            "write_file",
            {"path": "browser-proof.txt", "content": "approved from browser"},
            call_id="browser-write",
        )
    content = "# Verified response\n\nA **streamed** answer with a table.\n\n| Check | Result |\n| --- | --- |\n| Native worker | Ready |\n\nInline $x^2 + y^2 = z^2$ and \\(a+b\\).\n\n$$\\int_0^1 x \\, dx = \\frac{1}{2}$$\n\n```python\nprint('hello')\n```\n\nPrices $5 and $10. `<script>bad()</script>`\n\n![blocked](https://example.com/tracker.png)\n\n[unsafe](javascript:alert(1))\n"
    if "Long continuity probe" in prompt:
        content = (
            "Stable opening paragraph for selection and node identity.\n\n"
            "```python\nprint('stable copy control')\n```\n\n"
            "| Stable | Table |\n| --- | --- |\n| early | row |\n\n"
            + "Long retained body sentence. " * 360
            + "\n\n[late reference][continuity]\n\n"
            + "```text\nunfinished-looking content retained safely\n```\n\n"
            + "[continuity]: https://example.com/continuity\n"
        )
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
        delay=0.02 if "Long continuity probe" in prompt else 0.08,
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
    # A real image for the read-path attachment scenario ("Image probe").
    (project / "shot.png").write_bytes(
        base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9Q"
            "DwADhgGAWjR9awAAAABJRU5ErkJggg=="
        )
    )
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
