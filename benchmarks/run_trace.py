#!/usr/bin/env python3
"""Measure one µAgent run: a timed command and privacy-safe trace aggregates.

Shared by `eval.py` and `audit.py`. A caller always names the executable it
measures; nothing here resolves a binary by itself.
"""

from __future__ import annotations

import collections
import json
import re
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any


def measured_command(binary: Path, arguments: list[str]) -> list[str]:
    timer = Path("/usr/bin/time")
    if not timer.is_file():
        return [str(binary), *arguments]
    if sys.platform == "darwin":
        return [str(timer), "-l", str(binary), *arguments]
    if sys.platform.startswith("linux"):
        return [str(timer), "-v", str(binary), *arguments]
    return [str(binary), *arguments]


def peak_rss(stderr: str) -> int:
    macos = re.search(r"(\d+)\s+maximum resident set size", stderr)
    if macos:
        return int(macos.group(1))
    linux = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", stderr)
    return int(linux.group(1)) * 1024 if linux else 0


def read_trace(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    records = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(record, dict):
            records.append(record)
    return records


def trace_metrics(
    records: list[dict[str, Any]],
    *,
    provenance_fn: Callable[[Any], Any] | None = None,
    cohort_fn: Callable[[Any], Any] | None = None,
) -> dict[str, Any]:
    """Reconstruct per-request context and privacy-safe trajectory aggregates.

    `provenance_fn`/`cohort_fn` stay injectable because provenance redaction
    lives with the session-journal reader in `benchmarks/`, which an installed
    skill cannot import.
    """
    events: collections.Counter[str] = collections.Counter()
    batches: list[int] = []
    calls: list[dict[str, Any]] = []
    context_bytes: list[int] = []
    result_chars_by_tool: collections.Counter[str] = collections.Counter()
    issue_codes: collections.Counter[str] = collections.Counter()
    current_messages = 0
    first: dict[str, Any] = {}
    compactions = 0
    model_duration_ms = 0.0
    request_preparation_ms = 0.0
    tool_result_chars = 0
    tool_result_texts: list[str] = []
    browser_snapshot_chars = 0
    browser_calls: set[str] = set()
    pending_failures: collections.Counter[tuple[Any, str]] = collections.Counter()
    failed_call_recoveries = 0
    failed_calls = 0
    usage: collections.Counter[str] = collections.Counter()
    provenance = None
    route = ""
    for record in records:
        name = str(record.get("event", ""))
        data = record.get("data", {}) or {}
        events[name] += 1
        if name == "session_ready":
            raw = data.get("provenance")
            provenance = provenance_fn(raw) if provenance_fn else None
            route = str(data.get("route") or "")
        elif name == "model_request":
            first = first or data
            batches.append(0)
            if data.get("projected_context"):
                message_bytes = int(data.get("message_bytes") or 0)
            elif "message_bytes" in data:
                current_messages = int(data.get("message_bytes") or 0)
                message_bytes = current_messages
            else:
                current_messages += int(data.get("new_message_bytes") or 0)
                message_bytes = current_messages
            schema_bytes = int(data.get("schema_bytes") or 0)
            context_bytes.append(message_bytes + schema_bytes)
        elif name == "model_response":
            model_duration_ms += float(data.get("end_to_end_ms") or data.get("duration_ms") or 0)
            request_preparation_ms += float(data.get("request_preparation_ms") or 0)
        elif name == "tool_call":
            arguments = data.get("arguments", {})
            if isinstance(arguments, str):
                try:
                    arguments = json.loads(arguments)
                except json.JSONDecodeError:
                    arguments = {}
            call = {
                "occurrence_id": str(data.get("occurrence_id") or ""),
                "turn": data.get("turn"),
                "step": data.get("step"),
                "name": data.get("name"),
                "arguments": arguments,
            }
            calls.append(call)
            if call["name"] == "run" and isinstance(arguments, dict):
                if "playwright-cli" in str(arguments.get("command", "")):
                    browser_calls.add(call["occurrence_id"])
            if batches:
                batches[-1] += 1
        elif name == "tool_result":
            chars = int(data.get("result_chars") or 0)
            tool_result_texts.append(str(data.get("result") or ""))
            tool_name = str(data.get("name") or "?")
            tool_result_chars += chars
            result_chars_by_tool[tool_name] += chars
            if str(data.get("occurrence_id") or "") in browser_calls:
                browser_snapshot_chars += chars
            issue = str(data.get("issue_code") or "")
            if issue:
                issue_codes[issue] += 1
            key = (data.get("turn"), tool_name)
            status = str(data.get("status") or "")
            if status not in ("ok", "succeeded", "success"):
                failed_calls += 1
                pending_failures[key] += 1
            elif pending_failures[key]:
                failed_call_recoveries += 1
                pending_failures[key] = 0
        elif name == "turn_end":
            turn_usage = data.get("usage") or {}
            if isinstance(turn_usage, dict):
                for field_name in (
                    "input",
                    "output",
                    "cache_read",
                    "cache_write",
                    "reasoning",
                    "web_searches",
                ):
                    usage[field_name] += int(turn_usage.get(field_name) or 0)
                usage["cost"] += float(turn_usage.get("cost") or 0)
                if turn_usage.get("cost_reported"):
                    usage["cost_reported_turns"] += 1
        elif name == "compact_end" and data.get("outcome") == "ok":
            compactions += 1
    no_action = max(len([count for count in batches[:-1] if count == 0]), 0)
    cumulative_context_bytes = sum(context_bytes)
    return {
        "model_requests": len(batches),
        "tool_calls": len(calls),
        "calls": calls,
        "max_batch": max(batches, default=0),
        "no_action_rounds": no_action,
        "compactions": compactions,
        "events": events,
        "cumulative_estimated_context_bytes": cumulative_context_bytes,
        "estimated_context_bytes_progression": context_bytes,
        "max_estimated_context_bytes": max(context_bytes, default=0),
        "initial_schema_bytes": int(first.get("schema_bytes") or 0),
        "tool_result_chars": tool_result_chars,
        "tool_result_text": "\n".join(tool_result_texts),
        "tool_result_chars_by_tool": result_chars_by_tool,
        "model_duration_ms": model_duration_ms,
        "request_preparation_ms": request_preparation_ms,
        "usage": usage,
        "issue_codes": issue_codes,
        "failed_calls": failed_calls,
        "failed_call_recoveries": failed_call_recoveries,
        "unrecovered_failed_calls": sum(pending_failures.values()),
        "browser_commands": len(browser_calls),
        "browser_snapshot_chars": browser_snapshot_chars,
        "provenance": provenance,
        "cohort": cohort_fn(provenance) if cohort_fn else "",
        "route": route,
    }
