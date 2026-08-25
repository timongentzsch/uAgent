#!/usr/bin/env python3
"""What real sessions did, and where they wasted time, tokens and turns.

The system prompt asks for batched independent calls, dedicated tools over
`run`, and delegation for orthogonal work. Whether that lands is measurable:
every session writes a `.events.jsonl` sidecar recording each tool call with
its turn and step, and calls sharing a (turn, step) were issued in one batch.
Each result records its status, duration and size, so the same journals also
say which tools fail, which are slow, and which fill the context.

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
import sys

DELEGATION_TOOLS = {"subagent", "task"}


def journal_metrics(path):
    """One session: batches, tool mix, and the cost each tool actually incurred."""
    summary = {
        "batches": collections.Counter(),
        "names": collections.Counter(),
        "failures": collections.Counter(),
        "duration_ms": collections.Counter(),
        "result_chars": collections.Counter(),
        "results": 0,
    }
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
            elif record.get("type") == "tool.result":
                summary["results"] += 1
                status = data.get("status", "?")
                if status not in ("ok", "succeeded"):
                    summary["failures"][(name, status)] += 1
                summary["duration_ms"][name] += float(data.get("duration_ms") or 0)
                summary["result_chars"][name] += int(data.get("result_chars") or 0)
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
        "duration_ms": collections.Counter(),
        "result_chars": collections.Counter(),
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
        for field in ("names", "failures", "duration_ms", "result_chars"):
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

    print("\ntime spent in tools (seconds)")
    for name, milliseconds in total["duration_ms"].most_common(6):
        print(f"  {name:<20} {milliseconds / 1000:8.1f}")

    print("\ncontext filled by tool results (chars)")
    for name, chars in total["result_chars"].most_common(6):
        print(f"  {name:<20} {chars:>10,}")


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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--history",
        default=os.path.expanduser("~/.uagent/history"),
        help="session history directory",
    )
    parser.add_argument("--since", help="only sessions modified on or after YYYY-MM-DD")
    parser.add_argument("--json", help="write the same summary as JSON")
    arguments = parser.parse_args()

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
