#!/usr/bin/env python3
"""Behavioral metrics from saved session journals.

The system prompt asks for batched independent calls, dedicated tools over
`run`, and delegation for orthogonal work. Whether that lands is measurable:
every session writes a `.events.jsonl` sidecar recording each tool call with
its turn and step, and calls sharing a (turn, step) were issued in one batch.

Usage:
    python3 benchmarks/session_metrics.py [--history DIR] [--since YYYY-MM-DD]

A/B is not available across a prompt change — journals are written by whichever
prompt was live at the time — so record a baseline, change the prompt, then
compare a later cohort of sessions against it.
"""

import argparse
import collections
import json
import os
import pathlib
import sys

DELEGATION_TOOLS = {"subagent", "task", "advisor"}


def journal_metrics(path):
    """Tool calls in one session, grouped into the batches they were sent in."""
    batches = collections.Counter()
    names = collections.Counter()
    with open(path, encoding="utf-8") as journal:
        for line in journal:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("type") != "tool.call":
                continue
            data = record.get("data", {})
            batches[(data.get("turn"), data.get("step"))] += 1
            names[data.get("name", "?")] += 1
    return batches, names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--history",
        default=os.path.expanduser("~/.uagent/history"),
        help="session history directory",
    )
    parser.add_argument("--since", help="only sessions modified on or after YYYY-MM-DD")
    arguments = parser.parse_args()

    since = 0.0
    if arguments.since:
        import datetime

        since = datetime.datetime.fromisoformat(arguments.since).timestamp()

    journals = sorted(
        pathlib.Path(arguments.history).glob("*/*.events.jsonl"),
        key=os.path.getmtime,
        reverse=True,
    )
    sizes = collections.Counter()
    names = collections.Counter()
    sessions = 0
    calls = 0
    batch_total = 0
    for journal in journals:
        if os.path.getmtime(journal) < since:
            continue
        batches, session_names = journal_metrics(journal)
        if not batches:
            continue
        sessions += 1
        calls += sum(batches.values())
        batch_total += len(batches)
        sizes.update(batches.values())
        names.update(session_names)

    if not sessions:
        print("no journalled sessions found", file=sys.stderr)
        return 1

    print(f"sessions           {sessions}")
    print(f"tool calls         {calls}")
    print(f"batches            {batch_total}")
    print(f"mean batch size    {calls / batch_total:.2f}")
    print(f"batch sizes        {sorted(sizes.items())}")
    delegations = sum(names[name] for name in DELEGATION_TOOLS)
    print(f"delegations        {delegations}")
    for name, count in names.most_common(10):
        print(f"  {name:<20} {count:>5}  {100 * count / calls:4.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
