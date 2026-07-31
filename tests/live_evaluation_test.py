#!/usr/bin/env python3
"""Unit checks for the live workflow evaluator's general diagnostics."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path

SCRIPT = Path(__file__).with_name("agent_workflow_live.py")
SPEC = importlib.util.spec_from_file_location("agent_workflow_live", SCRIPT)
assert SPEC and SPEC.loader
live = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = live
SPEC.loader.exec_module(live)


def event(event_name: str, **data: object) -> dict[str, object]:
    return {"event": event_name, "data": data}


def main() -> int:
    events = [
        event("model_response", duration_ms=100, first_event_ms=70, content="", tool_calls=[{}]),
        event("tool_batch", parallel=True),
        event("tool_result", status="error", duration_ms=5),
        event("tool_result", status="error", duration_ms=7),
        event(
            "tool_call",
            name="run_python",
            arguments={"packages": ["example"], "code": "print('x')"},
        ),
        event("tool_result", status="error", duration_ms=9),
        event("tool_failure_advisory", consecutive_failures=3),
        event("tool_call", name="grep", arguments={"pattern": "needle", "path": "."}),
        event(
            "tool_result",
            name="grep",
            status="ok",
            duration_ms=1,
            result="[ripgrep · 1 result lines]\nfile:1:needle",
        ),
        event(
            "model_response",
            duration_ms=50,
            first_event_ms=20,
            content="- one\n- two",
            tool_calls=[],
        ),
    ]
    metrics = live.trace_metrics(events)
    assert metrics["api_wait_ms"] == 90
    assert metrics["api_generation_ms"] == 60
    assert metrics["tool_time_ms"] == 22
    assert metrics["failed_tools"] == 3
    assert metrics["max_consecutive_failed_tools"] == 3
    assert metrics["parallel_batches"] == 1
    assert metrics["failure_advisories"] == 1
    assert len(metrics["dependency_installs"]) == 1
    assert metrics["exploration"]["native_grep_calls"] == 1
    assert metrics["exploration"]["ripgrep_results"] == 1
    assert (
        live.final_response(
            [
                event("model_response", content="intermediate", tool_calls=[]),
                event("model_response", content="", tool_calls=[], error="upstream failure"),
            ]
        )
        == ""
    )

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "same.txt").write_text("same", encoding="utf-8")
        before = live.workspace_snapshot(root)
        contract = live.check_contract(
            events,
            before,
            live.workspace_snapshot(root),
            bullets=2,
            max_words=2,
            read_only=True,
            forbid_dependency_installs=True,
        )
        assert not contract["passed"]
        assert contract["bullet_count"] == 2
        assert contract["word_count"] == 2
        assert contract["workspace_changes"] == []
        assert contract["violations"] == [
            "dependency installation used in pinned fixture (1 calls)"
        ]

        (root / "same.txt").write_text("changed", encoding="utf-8")
        changed = live.check_contract(events, before, live.workspace_snapshot(root), read_only=True)
        assert changed["workspace_changes"] == ["same.txt"]
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
