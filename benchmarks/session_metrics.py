#!/usr/bin/env python3
"""Measure real µAgent trajectories without retaining prompts or tool values.

Every session sidecar records privacy-safe tool, turn and resource metadata.
This report groups sessions by the canonical ``session.ready`` provenance,
then derives recovery and repetition labels offline: runtime telemetry records
facts, never a judgement about whether a trajectory was good.

Usage:
    python3 benchmarks/session_metrics.py [--history DIR] [--since YYYY-MM-DD]
    python3 benchmarks/session_metrics.py --cohort 3f22a10c9c73
    python3 benchmarks/session_metrics.py --json /tmp/sessions.json

Journals predating provenance are an explicit ``legacy`` cohort. Failure text
is read only from the adjacent transcript and folded before aggregation; raw
arguments, outputs, routes, commands and credentials never enter the report.
"""

import argparse
import collections
import hashlib
import json
import os
import pathlib
import re
import sys

DELEGATION_TOOLS = {"subagent", "task"}
ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURE_ROOT = ROOT / "tests" / "fixtures" / "sessions"
VARIABLE_PATH = re.compile(r"(?:/|\./)[^\s`'\";,]+")
VARIABLE_NUMBER = re.compile(r"\d+")
SECRET_ASSIGNMENT = re.compile(
    r"(?i)\b(api[_-]?key|token|secret|password|credential)\s*[:=]\s*[^\s,;]+"
)
BEARER_SECRET = re.compile(r"(?i)\bbearer\s+[^\s,;]+")
MESSAGE_CHARS = 110
PROVENANCE_FIELDS = (
    "format",
    "binary_version",
    "build_id",
    "prompt_digest",
    "surface_digest",
    "active_schema_digest",
    "toolset",
)
BEHAVIOR_FIELDS = (
    "reasoning_effort",
    "openrouter_variant",
    "context_window",
    "memory",
    "memory_generate",
    "run_mode",
    "approval",
    "auto_compact_pct",
    "auto_compact_tokens",
    "tool_concurrency",
    "tool_result_chars",
    "tool_batch_result_chars",
    "steering",
    "adaptive_system",
    "max_tokens",
    "prompt_overlay",
)
USAGE_FIELDS = (
    "input",
    "output",
    "cache_read",
    "cache_write",
    "reasoning",
    "web_searches",
)
OK_STATUSES = {"ok", "succeeded", "success"}


def transcript_text(journal):
    """Tool-result text from the transcript beside a journal, keyed by call id."""
    transcript = journal.with_name(journal.name[: -len(".events.jsonl")])
    text = {}
    if not transcript.exists():
        return text
    with open(transcript, encoding="utf-8", errors="replace") as conversation:
        for line in conversation:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            pending = [record]
            while pending:
                node = pending.pop()
                if isinstance(node, dict):
                    call = node.get("tool_use_id") or node.get("tool_call_id")
                    if call:
                        body = node.get("content")
                        if isinstance(body, list):
                            body = " ".join(
                                part.get("text", "") for part in body if isinstance(part, dict)
                            )
                        text[call] = "" if body is None else str(body)
                    pending.extend(node.values())
                elif isinstance(node, list):
                    pending.extend(node)
    return text


def message_key(text):
    """One redacted failure message, folded so the same failure groups."""
    first = text.strip().splitlines()[0] if text.strip() else ""
    first = SECRET_ASSIGNMENT.sub(lambda match: f"{match.group(1)}=<redacted>", first)
    first = BEARER_SECRET.sub("Bearer <redacted>", first)
    folded = VARIABLE_NUMBER.sub("N", VARIABLE_PATH.sub("<path>", first))
    return folded[:MESSAGE_CHARS]


def safe_provenance(value):
    """Allowlist cohort fields so a malformed journal cannot leak a secret."""
    if not isinstance(value, dict) or not value.get("format"):
        return None
    safe = {
        key: value[key]
        for key in PROVENANCE_FIELDS
        if key in value and isinstance(value[key], (str, int, float, bool))
    }
    behavior = value.get("behavior")
    if isinstance(behavior, dict):
        selected = {
            key: behavior[key]
            for key in BEHAVIOR_FIELDS
            if key in behavior
            and (behavior[key] is None or isinstance(behavior[key], (str, int, float, bool)))
        }
        if selected:
            safe["behavior"] = selected
    return safe if safe.get("format") else None


def provenance_cohort(provenance):
    if provenance is None:
        return "legacy"
    canonical = json.dumps(provenance, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode()).hexdigest()[:12]


def journal_metrics(path):
    """One session's privacy-safe trajectory and resource observations."""
    summary = {
        "batches": collections.Counter(),
        "names": collections.Counter(),
        "failures": collections.Counter(),
        "messages": collections.Counter(),
        "duration_ms": collections.Counter(),
        "result_chars": collections.Counter(),
        "repeats": collections.Counter(),
        "issues": collections.Counter(),
        "trajectory": collections.Counter(),
        "outcomes": collections.Counter(),
        "usage": collections.Counter(),
        "turn_duration_ms": 0.0,
        "context_tokens": [],
        "message_counts": [],
        "results": 0,
        "provenance": None,
        "cohort": "legacy",
    }
    failed_messages = []
    signatures = collections.Counter()
    pending_failures = collections.Counter()
    with open(path, encoding="utf-8", errors="replace") as journal:
        for line in journal:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            data = record.get("data", {}) or {}
            event_type = record.get("type")
            name = data.get("name", "?")
            if event_type == "session.ready" and summary["provenance"] is None:
                summary["provenance"] = safe_provenance(data.get("provenance"))
                summary["cohort"] = provenance_cohort(summary["provenance"])
            elif event_type == "tool.call":
                summary["batches"][(data.get("turn"), data.get("step"))] += 1
                summary["names"][name] += 1
                digest = data.get("arguments_digest")
                if digest:
                    signatures[(name, digest)] += 1
            elif event_type == "tool.result":
                summary["results"] += 1
                status = str(data.get("status", "?"))
                turn_tool = (data.get("turn"), name)
                issue = str(data.get("issue_code") or "")
                if issue:
                    summary["issues"][issue] += 1
                    summary["trajectory"]["argument_issues"] += 1
                if status not in OK_STATUSES:
                    kind = data.get("error_code") or status
                    summary["failures"][(name, kind)] += 1
                    summary["trajectory"]["failed_calls"] += 1
                    pending_failures[turn_tool] += 1
                    failed_messages.append((name, data.get("id")))
                elif pending_failures[turn_tool]:
                    summary["trajectory"]["failed_call_recoveries"] += 1
                    pending_failures[turn_tool] = 0
                operation = data.get("activity_operation")
                if operation in ("poll", "wait") and data.get("no_change"):
                    summary["trajectory"]["activity_no_change"] += 1
                if operation in ("poll", "wait") and data.get("activity_terminal"):
                    summary["trajectory"]["activity_terminal"] += 1
                summary["duration_ms"][name] += float(data.get("duration_ms") or 0)
                summary["result_chars"][name] += int(data.get("result_chars") or 0)
            elif event_type == "turn.completed":
                summary["outcomes"][str(data.get("outcome") or "unknown")] += 1
                summary["turn_duration_ms"] += float(data.get("duration_ms") or 0)
                usage = data.get("usage") or {}
                if isinstance(usage, dict):
                    for field in USAGE_FIELDS:
                        summary["usage"][field] += int(usage.get(field) or 0)
                    summary["usage"]["cost"] += float(usage.get("cost") or 0)
                    if usage.get("cost_reported"):
                        summary["usage"]["cost_reported_turns"] += 1
                if data.get("context_tokens") is not None:
                    summary["context_tokens"].append(int(data.get("context_tokens") or 0))
                if data.get("messages") is not None:
                    summary["message_counts"].append(int(data.get("messages") or 0))
    for (name, _), count in signatures.items():
        if count > 1:
            summary["repeats"][name] += count - 1
            summary["trajectory"]["repeated_identical_calls"] += count - 1
    summary["trajectory"]["unrecovered_failed_calls"] = sum(pending_failures.values())
    if failed_messages:
        text = transcript_text(path)
        for name, call in failed_messages:
            key = message_key(text.get(call, ""))
            if key:
                summary["messages"][(name, key)] += 1
    return summary


def collect(history, since, cohorts=()):
    journals = sorted(
        pathlib.Path(history).glob("*/*.events.jsonl"),
        key=os.path.getmtime,
        reverse=True,
    )
    wanted = set(cohorts)
    total = {
        "sessions": 0,
        "calls": 0,
        "results": 0,
        "batch_total": 0,
        "sizes": collections.Counter(),
        "names": collections.Counter(),
        "failures": collections.Counter(),
        "messages": collections.Counter(),
        "duration_ms": collections.Counter(),
        "result_chars": collections.Counter(),
        "repeats": collections.Counter(),
        "issues": collections.Counter(),
        "trajectory": collections.Counter(),
        "outcomes": collections.Counter(),
        "usage": collections.Counter(),
        "cohorts": collections.Counter(),
        "provenance": {},
        "turn_duration_ms": 0.0,
        "max_context_tokens": 0,
        "final_context_tokens": 0,
        "max_messages": 0,
    }
    for journal in journals:
        if os.path.getmtime(journal) < since:
            continue
        summary = journal_metrics(journal)
        if not summary["batches"] or (wanted and summary["cohort"] not in wanted):
            continue
        total["sessions"] += 1
        total["calls"] += sum(summary["batches"].values())
        total["results"] += summary["results"]
        total["batch_total"] += len(summary["batches"])
        total["sizes"].update(summary["batches"].values())
        total["cohorts"][summary["cohort"]] += 1
        if summary["provenance"] is not None:
            total["provenance"][summary["cohort"]] = summary["provenance"]
        for field in (
            "names",
            "failures",
            "messages",
            "duration_ms",
            "result_chars",
            "repeats",
            "issues",
            "trajectory",
            "outcomes",
            "usage",
        ):
            total[field].update(summary[field])
        total["turn_duration_ms"] += summary["turn_duration_ms"]
        if summary["context_tokens"]:
            total["max_context_tokens"] = max(
                total["max_context_tokens"], max(summary["context_tokens"])
            )
            total["final_context_tokens"] += summary["context_tokens"][-1]
        if summary["message_counts"]:
            total["max_messages"] = max(total["max_messages"], max(summary["message_counts"]))
    return total


def report(total):
    calls = total["calls"]
    batches = total["batch_total"]
    print(f"sessions           {total['sessions']}")
    print(f"tool calls         {calls}")
    print(f"batches            {batches}")
    print(f"mean batch size    {calls / batches:.2f}")
    batched = sum(count for size, count in total["sizes"].items() if size > 1)
    print(f"batched steps      {100 * batched / batches:.0f}%")
    print(f"batch sizes        {sorted(total['sizes'].items())}")
    delegations = sum(total["names"][name] for name in DELEGATION_TOOLS)
    print(f"delegations        {delegations}")

    print("\nprovenance cohorts")
    for cohort, count in total["cohorts"].most_common():
        provenance = total["provenance"].get(cohort, {})
        identity = provenance.get("build_id") or provenance.get("binary_version") or "pre-v1"
        print(f"  {cohort:<12} {count:>4}  {identity}")

    print("\ntool mix (share of calls)")
    for name, count in total["names"].most_common(10):
        print(f"  {name:<20} {count:>5}  {100 * count / calls:4.1f}%")

    failures = sum(total["failures"].values())
    share = 100 * failures / total["results"] if total["results"] else 0
    print(f"\nnon-ok results     {failures} of {total['results']} ({share:.1f}%)")
    for (name, status), count in total["failures"].most_common(8):
        print(f"  {name:<20} {status:<20} {count:>4}")
    if total["issues"]:
        print(f"argument issues    {dict(total['issues'].most_common())}")

    if total["messages"]:
        print("\nwhat those failures said (paths and counts folded)")
        for (name, text), count in total["messages"].most_common(10):
            print(f"  {count:>4}x {name:<12} {text}")
    else:
        print("\nfailure messages   no transcript beside these journals")

    print("\ntrajectory labels (derived offline)")
    for name, count in total["trajectory"].most_common():
        print(f"  {name:<30} {count:>6}")
    print(f"  turn outcomes                  {dict(total['outcomes'])}")

    print("\ntime spent in tools (seconds)")
    for name, milliseconds in total["duration_ms"].most_common(6):
        print(f"  {name:<20} {milliseconds / 1000:8.1f}")
    print(f"  model turns          {total['turn_duration_ms'] / 1000:8.1f}")

    print("\ncontext and usage")
    print(f"  max observed context          {total['max_context_tokens']:>10,} tokens")
    print(f"  summed final context          {total['final_context_tokens']:>10,} tokens")
    print(f"  max messages                  {total['max_messages']:>10,}")
    for field in USAGE_FIELDS:
        print(f"  {field:<28} {int(total['usage'][field]):>10,}")
    print(f"  reported cost                 ${float(total['usage']['cost']):.6f}")

    print("\ncontext filled by tool results (chars)")
    for name, chars in total["result_chars"].most_common(6):
        print(f"  {name:<20} {chars:>10,}")


def serialisable(value):
    """Recursively turn tuple-keyed counters into JSON objects."""
    if isinstance(value, collections.Counter):
        return {
            "|".join(map(str, key)) if isinstance(key, tuple) else str(key): serialisable(count)
            for key, count in value.items()
        }
    if isinstance(value, dict):
        return {str(key): serialisable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [serialisable(item) for item in value]
    return value


def self_test():
    """Planted mixed cohorts make every new classifier prove it can go red."""
    total = collect(FIXTURE_ROOT, 0.0)
    expected_message = (
        "edit_file",
        "error: edit N `old` not found in <path>; nearby current line N:",
    )
    failures = []
    if total["sessions"] != 3:
        failures.append(f"read {total['sessions']} planted sessions, want 3")
    if total["messages"][expected_message] != 2:
        failures.append(
            f"planted message grouped {total['messages'][expected_message]} times, want 2"
        )
    if any(name == "read_path" for name, _ in total["messages"]):
        failures.append("a successful call was reported as a failure")
    redacted = message_key("error: api_key=private-value Bearer second-private-value")
    if "private-value" in redacted or "second-private-value" in redacted:
        failures.append(f"failure-message secret was retained: {redacted}")
    if total["cohorts"]["legacy"] != 1 or len(total["cohorts"]) != 3:
        failures.append(f"mixed provenance cohorts were not separated: {dict(total['cohorts'])}")
    expected_trajectory = {
        "failed_calls": 2,
        "failed_call_recoveries": 1,
        "unrecovered_failed_calls": 1,
        "repeated_identical_calls": 1,
        "argument_issues": 1,
        "activity_no_change": 1,
        "activity_terminal": 1,
    }
    for name, expected in expected_trajectory.items():
        if total["trajectory"][name] != expected:
            failures.append(f"trajectory {name}={total['trajectory'][name]}, want {expected}")
    if total["issues"]["schema.type"] != 1:
        failures.append(f"schema issue count is {total['issues']['schema.type']}, want 1")
    if total["outcomes"] != collections.Counter({"answered": 2, "error": 1}):
        failures.append(f"turn outcomes differ: {dict(total['outcomes'])}")
    print(f"planted cohorts            {dict(total['cohorts'])}")
    print(f"planted trajectory labels  {dict(total['trajectory'])}")
    print(f"planted failure messages   {dict(total['messages'])}")
    for failure in failures:
        print(f"SELF-TEST FAILED: {failure}")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--history",
        default=os.path.expanduser("~/.uagent/history"),
        help="session history directory",
    )
    parser.add_argument("--since", help="only sessions modified on or after YYYY-MM-DD")
    parser.add_argument("--cohort", action="append", default=[], help="cohort id or legacy")
    parser.add_argument("--json", help="write the same privacy-safe summary as JSON")
    parser.add_argument(
        "--self-test", action="store_true", help="check classifiers against planted fixtures"
    )
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()

    since = 0.0
    if arguments.since:
        import datetime

        since = datetime.datetime.fromisoformat(arguments.since).timestamp()

    total = collect(arguments.history, since, arguments.cohort)
    if not total["sessions"]:
        print("no journalled sessions found for the selected cohort", file=sys.stderr)
        return 1
    report(total)
    if arguments.json:
        pathlib.Path(arguments.json).write_text(
            json.dumps(serialisable(total), indent=2) + "\n", encoding="utf-8"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
