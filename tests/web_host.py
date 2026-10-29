#!/usr/bin/env python3
"""Mock-backed host for browser tests; never calls an external model."""

import argparse
import base64
import json
import os
import pathlib
import signal
import tempfile
import threading

from integration_support import (
    OPENROUTER_SUPPORTS,
    Server,
    event,
    save_settings,
    tool_call,
    write_json_response,
    write_sse_sequence,
)
from web_support import web_host

parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=pathlib.Path, required=True)
parser.add_argument("--port", type=int, default=8765)
parser.add_argument("--fixture", type=pathlib.Path, required=True)
parser.add_argument("--no-model", action="store_true")
# Approval is Auto and a decision model on the mock provider is sure but
# worried about where the data goes.
parser.add_argument("--auto-review", action="store_true")
args = parser.parse_args()
stop = threading.Event()
signal.signal(signal.SIGINT, lambda *_: stop.set())
signal.signal(signal.SIGTERM, lambda *_: stop.set())


def decide(handler):
    """The decisions endpoint: allow is likely, sending data away is too."""
    noul = {"destructive": 0.0, "beyond_request": 0.0, "touches_secrets": 0.0}
    noul["sends_data_off_machine"] = 0.78
    answers = {name: {"type": "noul", "noul": probability} for name, probability in noul.items()}
    answers["permission"] = {
        "type": "choice",
        "choice": "allow",
        "probabilities": {"allow": 0.99, "ask": 0.01, "deny": 0.0},
    }
    write_json_response(handler, {"answers": answers, "usage": {"input_tokens": 9, "cost": 0.0001}})


def answer(handler, body):
    if "questions" in body:
        return decide(handler)
    texts = [
        message.get("content", "") for message in body["messages"] if message.get("role") == "user"
    ]
    prompt = str(texts[-1]) if texts else ""
    expected_model = "model-a" if prompt == "Retained follow-up" else "model-b"
    assert body.get("model") == expected_model, body.get("model")
    if "for a fresh agent context" in str(body["messages"]):
        return event({"content": "COMPACT-PREVIEW-SUMMARY"})
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
    if "Plan probe" in prompt:
        items = [
            {"text": "Read the parser", "status": "done"},
            {"text": "Fix the lexer", "status": "in_progress"},
            {"text": "Add tests", "status": "pending"},
        ]
        return tool_call("plan", {"items": items}, call_id="plan-write")
    if "Unknown tool probe" in prompt and not any(
        message.get("tool_call_id") == "unknown-tool" for message in body["messages"]
    ):
        return tool_call("read_paths", {"path": "/etc"}, call_id="unknown-tool")
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
    if "Long diff probe" in prompt and not any(
        message.get("tool_call_id") == "long-write" for message in body["messages"]
    ):
        lines = [f"line {index:04d} of a long file" for index in range(2000)]
        return tool_call(
            "write_file",
            {"path": "long.txt", "content": "\n".join([*lines, "LAST_DIFF_LINE", ""])},
            call_id="long-write",
        )
    if "request approval" in prompt and not any(
        message.get("role") == "tool" for message in body["messages"]
    ):
        return tool_call(
            "write_file",
            {"path": "browser-proof.txt", "content": "approved from browser"},
            call_id="browser-write",
        )
    if prompt == "Side tool probe":
        if not any(message.get("tool_call_id") == "btw-write" for message in body["messages"]):
            return tool_call(
                "write_file",
                {"path": "btw-proof.txt", "content": "side proof"},
                call_id="btw-write",
            )
        return event({"content": "SIDE-TOOL-COMPLETE"})
    if prompt == "Side followup probe":
        assert any(
            "SIDE-TOOL-COMPLETE" in str(message.get("content", "")) for message in body["messages"]
        ), "side history was lost"
        return event({"content": "SIDE-FOLLOWUP-COMPLETE"})
    content = "# Verified response\n\nA **streamed** answer with a table.\n\n| Check | Result |\n| --- | --- |\n| Native worker | Ready |\n\nInline $x^2 + y^2 = z^2$ and \\(a+b\\).\n\n$$\\int_0^1 x \\, dx = \\frac{1}{2}$$\n\n```python\nprint('hello')\n```\n\nPrices $5 and $10. `<script>bad()</script>`\n\n![blocked](https://example.com/tracker.png)\n\n[unsafe](javascript:alert(1))\n"
    if "Queue controls probe" in prompt:
        write_sse_sequence(
            handler, [event({"content": "Waiting for guidance. "}, finish=None)] * 200, delay=0.1
        )
        return None
    step, delay = 80, 0.08
    if "Long continuity probe" in prompt:
        delay = 0.02
        content = (
            "Stable opening paragraph for selection and node identity.\n\n"
            "```python\nprint('stable copy control')\n```\n\n"
            "| Stable | Table |\n| --- | --- |\n| early | row |\n\n"
            + "Long retained body sentence. " * 360
            + "\n\n[late reference][continuity]\n\n"
            + "```text\nunfinished-looking content retained safely\n```\n\n"
            + "[continuity]: https://example.com/continuity\n"
        )
    # Block structure decided by a later line (table rows, setext underlines,
    # markers before their word), cut into deltas that end anywhere.
    if "Structured stream probe" in prompt:
        step, delay = 5, 0.02
        content = (
            "Here is what I found:\n- The **web** client streams\n- Each block parses\n"
            "1. First point\n2. Second with `code`\n"
            "Steps:\n- open the app\n- send a message\n\n"
            "| File | Lines | Notes |\n| --- | --- | --- |\n| chat.tsx | 120 | transcript |\n"
            "| style.css | 1788 | tokens |\n\n"
            "### Details\nA paragraph long enough to wrap on a phone, so wrapping moves.\n"
            "Another paragraph:\n---\nAfter a rule.\n\n> A quoted note\n> on two lines\n\n"
        ) * 3
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
                event({"content": content[index : index + step]}, finish=None)
                for index in range(0, len(content), step)
            ],
        ],
        delay=delay,
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
            "supports": OPENROUTER_SUPPORTS,
            "models": {"main": {"id": "model-a", "effort": "high"}},
        }
    }
    if args.auto_review:
        providers["mock"]["supports"] = [*OPENROUTER_SUPPORTS, "decisions"]
        providers["mock"]["options"] = {
            "decisions_url": provider.url + "/decisions",
            "decisions_model": "mock-judge",
            "decision_bars": {"mock-judge": {"allow": 0.6, "deny": 0.95, "veto": 0.7}},
        }
    # Saved, not exported: what the environment sets cannot be edited in
    # Settings, and the providers are edited there.
    save_settings(home, binary=args.binary, UAGENT_PROVIDERS=json.dumps(providers))
    with web_host(
        args.binary.resolve(),
        root,
        home,
        provider.url,
        args.port,
        extra_env={
            "UAGENT_MODEL": None if args.no_model else "mock/main:high",
            # The terminal's shell, the same on every machine.
            "SHELL": "/bin/sh",
            **({"UAGENT_APPROVAL": "auto"} if args.auto_review else {}),
            # Locally the host serves the bundle on disk, so a web edit needs
            # no rebuild of the binary; CI serves the embedded one.
            **(
                {}
                if os.environ.get("CI")
                else {
                    "UAGENT_INTERNAL_WEB_DIST": str(
                        pathlib.Path(__file__).resolve().parents[1] / "web" / "dist"
                    )
                }
            ),
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
        # Until told to stop, or until the test runner that started this is
        # gone: a runner that was killed tells nobody.
        parent = os.getppid()
        while not stop.wait(1) and os.getppid() == parent:
            pass
