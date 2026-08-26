#!/usr/bin/env python3
"""What real sessions did, and where they wasted time, tokens and turns.

The system prompt asks for batched independent calls, dedicated tools over
`run`, and delegation for orthogonal work. Whether that lands is measurable:
every session writes a `.events.jsonl` sidecar recording each tool call with
its turn and step, and calls sharing a (turn, step) were issued in one batch.
Each result records its status, duration and size, so the same journals also
say which tools fail, which are slow, and which fill the context.

The journal records an error code, never the text. A code separates a bad
argument from a missing file; only the text says whether the harness caused
the failure — an argument the schema never allowed, a message naming a tool
that no longer exists — or whether a command the model wrote simply failed.
That distinction decides what is worth fixing, so the failing results are
joined back to the transcript beside each journal.

This is the first step of an improvement iteration: it names the paths worth
optimising and the failure modes worth fixing, from real usage rather than from
a guess about it.

Usage:
    python3 benchmarks/session_metrics.py [--history DIR] [--since YYYY-MM-DD]
    python3 benchmarks/session_metrics.py --json /tmp/sessions.json

A/B is not available across a prompt change — journals are written by whichever
prompt was live at the time — so record a baseline, change the prompt, then
compare a later cohort of sessions against it. History also outlives releases:
a name here may belong to a tool that has since been renamed or removed.
"""

import argparse
import collections
import json
import os
import pathlib
import re
import sys

DELEGATION_TOOLS = {"subagent", "task"}
ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURE_ROOT = ROOT / "tests" / "fixtures" / "sessions"
# Paths and counts differ on every occurrence of the same failure, so they are
# folded away before grouping. Nothing else is: a message that reads oddly here
# reads exactly as oddly to the model that received it.
VARIABLE_PATH = re.compile(r"(?:/|\./)[^\s`'\";,]+")
VARIABLE_NUMBER = re.compile(r"\d+")
MESSAGE_CHARS = 110


def transcript_text(journal):
    """Tool-result text from the transcript beside a journal, keyed by call id.

    The transcript is the conversation as the model saw it, so this is the
    failure the model actually had to recover from, not a reconstruction.
    """
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
    """One failure message, folded so the same failure groups with itself."""
    first = text.strip().splitlines()[0] if text.strip() else ""
    folded = VARIABLE_NUMBER.sub("N", VARIABLE_PATH.sub("<path>", first))
    return folded[:MESSAGE_CHARS]


def journal_metrics(path):
    """One session: batches, tool mix, and the cost each tool actually incurred."""
    summary = {
        "batches": collections.Counter(),
        "names": collections.Counter(),
        "failures": collections.Counter(),
        "messages": collections.Counter(),
        "duration_ms": collections.Counter(),
        "result_chars": collections.Counter(),
        "repeats": collections.Counter(),
        "results": 0,
    }
    failed = []
    signatures = collections.Counter()
    with open(path, encoding="utf-8") as journal:
        for line in journal:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            data = record.get("data", {}) or {}
            name = data.get("name", "?")
            if record.get("type") == "tool.call":
                summary["batches"][(data.get("turn"), data.get("step"))] += 1
                summary["names"][name] += 1
                # The journal keeps a digest, never the argument values, which
                # is enough to see the same call issued twice in one session.
                digest = data.get("arguments_digest")
                if digest:
                    signatures[(name, digest)] += 1
            elif record.get("type") == "tool.result":
                summary["results"] += 1
                status = data.get("status", "?")
                if status not in ("ok", "succeeded"):
                    # error_code names the failure; older journals predate it.
                    kind = data.get("error_code") or status
                    summary["failures"][(name, kind)] += 1
                    failed.append((name, data.get("id")))
                summary["duration_ms"][name] += float(data.get("duration_ms") or 0)
                summary["result_chars"][name] += int(data.get("result_chars") or 0)
    for (name, _), count in signatures.items():
        if count > 1:
            summary["repeats"][name] += count - 1
    if failed:
        text = transcript_text(path)
        for name, call in failed:
            key = message_key(text.get(call, ""))
            if key:
                summary["messages"][(name, key)] += 1
    return summary


def collect(history, since):
    journals = sorted(
        pathlib.Path(history).glob("*/*.events.jsonl"),
        key=os.path.getmtime,
        reverse=True,
    )
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
    }
    for journal in journals:
        if os.path.getmtime(journal) < since:
            continue
        summary = journal_metrics(journal)
        if not summary["batches"]:
            continue
        total["sessions"] += 1
        total["calls"] += sum(summary["batches"].values())
        total["results"] += summary["results"]
        total["batch_total"] += len(summary["batches"])
        total["sizes"].update(summary["batches"].values())
        for field in ("names", "failures", "messages", "duration_ms", "result_chars", "repeats"):
            total[field].update(summary[field])
    return total


def report(total):
    calls = total["calls"]
    print(f"sessions           {total['sessions']}")
    print(f"tool calls         {calls}")
    print(f"batches            {total['batch_total']}")
    print(f"mean batch size    {calls / total['batch_total']:.2f}")
    batched = sum(count for size, count in total["sizes"].items() if size > 1)
    print(f"batched steps      {100 * batched / total['batch_total']:.0f}%")
    print(f"batch sizes        {sorted(total['sizes'].items())}")
    delegations = sum(total["names"][name] for name in DELEGATION_TOOLS)
    print(f"delegations        {delegations}")

    print("\ntool mix (share of calls)")
    for name, count in total["names"].most_common(10):
        print(f"  {name:<20} {count:>5}  {100 * count / calls:4.1f}%")

    failures = sum(total["failures"].values())
    share = 100 * failures / total["results"] if total["results"] else 0
    print(f"\nnon-ok results     {failures} of {total['results']} ({share:.1f}%)")
    for (name, status), count in total["failures"].most_common(8):
        print(f"  {name:<20} {status:<20} {count:>4}")

    if total["messages"]:
        print("\nwhat those failures said (paths and counts folded)")
        for (name, text), count in total["messages"].most_common(10):
            print(f"  {count:>4}x {name:<12} {text}")
    else:
        print("\nfailure messages   no transcript beside these journals")

    print("\ntime spent in tools (seconds)")
    for name, milliseconds in total["duration_ms"].most_common(6):
        print(f"  {name:<20} {milliseconds / 1000:8.1f}")

    print("\ncontext filled by tool results (chars)")
    for name, chars in total["result_chars"].most_common(6):
        print(f"  {name:<20} {chars:>10,}")

    repeats = sum(total["repeats"].values())
    if repeats:
        print(f"\nrepeated identical calls within a session   {repeats}")
        for name, count in total["repeats"].most_common(6):
            print(f"  {name:<20} {count:>5}")
    else:
        print("\nrepeated identical calls   not recorded in these journals")


def serialisable(total):
    """Counters keyed by (tool, status) need string keys to survive JSON."""
    out = {}
    for key, value in total.items():
        if isinstance(value, collections.Counter):
            out[key] = {
                "|".join(map(str, name)) if isinstance(name, tuple) else str(name): count
                for name, count in value.items()
            }
        else:
            out[key] = value
    return out


def self_test():
    """Prove the message join fires, against a session planted for the purpose.

    A journal whose transcript is missing, renamed or shaped differently yields
    no messages at all, which on a clean cohort is indistinguishable from a
    cohort that had no failures. The fixtures carry two failures that differ
    only in path and number, so this also fails if the folding stops grouping
    them.
    """
    total = collect(FIXTURE_ROOT, 0.0)
    expected = ("edit_file", "error: edit N `old` not found in <path>; nearby current line N:")
    failures = []
    if total["sessions"] != 2:
        failures.append(f"read {total['sessions']} planted sessions, want 2")
    if total["messages"][expected] != 2:
        failures.append(f"planted message grouped {total['messages'][expected]} times, want 2")
    if any(name == "read_path" for name, _ in total["messages"]):
        failures.append("a successful call was reported as a failure")
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
    parser.add_argument("--json", help="write the same summary as JSON")
    parser.add_argument(
        "--self-test", action="store_true", help="check the message join against fixtures"
    )
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()

    since = 0.0
    if arguments.since:
        import datetime

        since = datetime.datetime.fromisoformat(arguments.since).timestamp()

    total = collect(arguments.history, since)
    if not total["sessions"]:
        print("no journalled sessions found", file=sys.stderr)
        return 1
    report(total)
    if arguments.json:
        pathlib.Path(arguments.json).write_text(
            json.dumps(serialisable(total), indent=2) + "\n", encoding="utf-8"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
