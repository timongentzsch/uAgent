#!/usr/bin/env python3
"""Provider-free trace metrics and scenario contract checks."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any


def read_events(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    events: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            events.append(value)
    return events


def session_usage(events: list[dict[str, Any]]) -> dict[str, Any]:
    for event in reversed(events):
        if event.get("event") == "session_end":
            usage = event.get("data", {}).get("usage", {})
            return usage if isinstance(usage, dict) else {}
    return {}


def raw_response_usage(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        event.get("data", {}).get("usage", {})
        for event in events
        if event.get("event") == "model_response"
        and isinstance(event.get("data", {}).get("usage"), dict)
    ]


def tool_events(events: list[dict[str, Any]], name: str) -> list[dict[str, Any]]:
    return [
        event
        for event in events
        if event.get("event") == "tool_result" and event.get("data", {}).get("name") == name
    ]


def tool_call_events(events: list[dict[str, Any]], name: str) -> list[dict[str, Any]]:
    return [
        event
        for event in events
        if event.get("event") == "tool_call" and event.get("data", {}).get("name") == name
    ]


def trace_contains(events: list[dict[str, Any]], text: str) -> bool:
    needle = text.lower()
    return any(needle in json.dumps(event.get("data", {})).lower() for event in events)


def peak_rss_bytes(stderr: str) -> int:
    match = re.search(r"(\d+)\s+maximum resident set size", stderr)
    if match:
        return int(match.group(1))
    match = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", stderr)
    return int(match.group(1)) * 1024 if match else 0


def terminal_outcome(events: list[dict[str, Any]]) -> tuple[str, str]:
    outcome = ""
    error = ""
    for event in events:
        data = event.get("data", {})
        if event.get("event") == "model_response" and data.get("error"):
            error = str(data["error"])
        if event.get("event") == "turn_end":
            outcome = str(data.get("outcome") or "")
    return outcome, error


def event_data(events: list[dict[str, Any]], name: str) -> list[dict[str, Any]]:
    return [
        event.get("data", {})
        for event in events
        if event.get("event") == name and isinstance(event.get("data"), dict)
    ]


def final_response(events: list[dict[str, Any]]) -> str:
    responses = event_data(events, "model_response")
    if not responses:
        return ""
    data = responses[-1]
    content = data.get("content")
    if not data.get("error") and not data.get("tool_calls") and isinstance(content, str):
        return content.strip()
    return ""


def parse_arguments(data: dict[str, Any]) -> dict[str, Any]:
    arguments = data.get("arguments", {})
    if isinstance(arguments, dict):
        return arguments
    if isinstance(arguments, str):
        try:
            parsed = json.loads(arguments or "{}", strict=False)
        except json.JSONDecodeError:
            return {}
        return parsed if isinstance(parsed, dict) else {}
    return {}


def workspace_snapshot(root: Path) -> dict[str, str]:
    snapshot: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            snapshot[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return snapshot


def dependency_install_calls(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    installs: list[dict[str, Any]] = []
    install_pattern = re.compile(
        r"(?:^|[;&|]\s*)(?:python\d*(?:\.\d+)?\s+-m\s+)?"
        r"(?:pip|pipx|uv)\s+(?:install|add|sync|run\b.*--with)",
        re.IGNORECASE,
    )
    for data in event_data(events, "tool_call"):
        args = parse_arguments(data)
        name = str(data.get("name") or "")
        packages = args.get("packages")
        command = str(args.get("command") or "")
        if (name == "run_python" and isinstance(packages, list) and packages) or (
            name == "run" and install_pattern.search(command)
        ):
            installs.append({"name": name, "packages": packages or [], "command": command})
    return installs


def trace_metrics(events: list[dict[str, Any]]) -> dict[str, Any]:
    responses = event_data(events, "model_response")
    tools = event_data(events, "tool_result")
    batches = event_data(events, "tool_batch")
    durations = [float(data.get("duration_ms") or 0) for data in responses]
    first_events = [float(data.get("first_event_ms") or 0) for data in responses]
    failures = [data for data in tools if data.get("status") != "ok"]
    calls = event_data(events, "tool_call")
    exploration_names = {"grep", "read_file", "list_dir"}
    exploration_calls = [data for data in calls if data.get("name") in exploration_names]
    exploration_signatures: list[str] = []
    for data in exploration_calls:
        exploration_signatures.append(
            f"{data.get('name')}:{json.dumps(parse_arguments(data), sort_keys=True)}"
        )
    repeated_exploration = len(exploration_signatures) - len(set(exploration_signatures))
    shell_searches = 0
    for data in calls:
        if data.get("name") != "run":
            continue
        command = str(parse_arguments(data).get("command") or "")
        if re.search(r"(?:^|[;&|]\s*)(?:rg|grep)\b", command):
            shell_searches += 1
    first_tool_step = min(
        (int(data.get("step") or 0) for data in calls),
        default=-1,
    )
    first_step_calls = (
        sum(int(data.get("step") or 0) == first_tool_step for data in calls)
        if first_tool_step >= 0
        else 0
    )
    calls_per_step: dict[int, int] = {}
    for data in calls:
        step = int(data.get("step") or 0)
        calls_per_step[step] = calls_per_step.get(step, 0) + 1
    consecutive = 0
    max_consecutive = 0
    for data in tools:
        if data.get("status") == "ok":
            consecutive = 0
        else:
            consecutive += 1
            max_consecutive = max(max_consecutive, consecutive)
    return {
        "api_wait_ms": round(sum(first_events), 3),
        "api_generation_ms": round(
            sum(
                max(0.0, duration - first)
                for duration, first in zip(durations, first_events, strict=True)
            ),
            3,
        ),
        "tool_time_ms": round(sum(float(data.get("duration_ms") or 0) for data in tools), 3),
        "failed_tools": len(failures),
        "max_consecutive_failed_tools": max_consecutive,
        "parallel_batches": sum(data.get("parallel") is True for data in batches),
        "tool_batches": len(batches),
        "failure_advisories": len(event_data(events, "tool_failure_advisory")),
        "dependency_installs": dependency_install_calls(events),
        "exploration": {
            "native_grep_calls": sum(data.get("name") == "grep" for data in calls),
            "ripgrep_results": sum(
                data.get("name") == "grep" and str(data.get("result") or "").startswith("[ripgrep")
                for data in tools
            ),
            "shell_rg_or_grep_calls": shell_searches,
            "read_file_calls": sum(data.get("name") == "read_file" for data in calls),
            "list_calls": sum(data.get("name") == "list_dir" for data in calls),
            "first_step_calls": first_step_calls,
            "max_calls_in_one_step": max(calls_per_step.values(), default=0),
            "repeated_identical_calls": repeated_exploration,
        },
    }


def check_contract(
    events: list[dict[str, Any]],
    before: dict[str, str],
    after: dict[str, str],
    *,
    bullets: int | None = None,
    max_words: int | None = None,
    read_only: bool = False,
    forbid_dependency_installs: bool = False,
) -> dict[str, Any]:
    answer = final_response(events)
    bullet_lines = [
        line for line in answer.splitlines() if re.match(r"^\s*(?:[-*+]|\d+[.)])\s+", line)
    ]
    words = re.findall(r"\b[\w'-]+\b", answer, flags=re.UNICODE)
    changed = sorted(
        path for path in before.keys() | after.keys() if before.get(path) != after.get(path)
    )
    installs = dependency_install_calls(events)
    violations: list[str] = []
    if bullets is not None and len(bullet_lines) != bullets:
        violations.append(f"expected {bullets} bullets, got {len(bullet_lines)}")
    if max_words is not None and len(words) > max_words:
        violations.append(f"expected at most {max_words} words, got {len(words)}")
    if read_only and changed:
        violations.append("workspace changed: " + ", ".join(changed[:8]))
    if forbid_dependency_installs and installs:
        violations.append(f"dependency installation used in pinned fixture ({len(installs)} calls)")
    return {
        "passed": not violations,
        "violations": violations,
        "bullet_count": len(bullet_lines),
        "word_count": len(words),
        "workspace_changes": changed,
        "dependency_installs": installs,
    }
