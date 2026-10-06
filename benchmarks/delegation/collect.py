#!/usr/bin/env python3
"""Collects every run under ~/uagent-eval/runs into results.json and prints a table."""

import collections
import glob
import json
import os

E = os.path.expanduser("~/uagent-eval")


def lines(path):
    out, bad = [], 0
    if not os.path.exists(path):
        return out, bad
    for line in open(path, errors="replace"):
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except Exception:
            bad += 1
    return out, bad


def session(path):
    events, _ = lines(path)
    name = os.path.basename(path).replace(".json.events.jsonl", "")
    turns = [e["data"] for e in events if e.get("type") == "turn.completed"]
    calls = collections.Counter()
    for e in events:
        if e.get("type") == "tool.call":
            d = e.get("data", {})
            calls[d.get("name") or d.get("tool") or "?"] += 1

    def use(k):
        return sum(t.get("usage", {}).get(k, 0) for t in turns)

    times = [e.get("time") for e in events if e.get("time")]
    return {
        "name": name,
        "kind": name.split("-")[0],
        "turns": len(turns),
        "outcomes": [t.get("outcome") for t in turns],
        "steps": sum(t.get("steps", 0) for t in turns),
        "tool_calls": sum(t.get("tool_calls", 0) for t in turns),
        "input": use("input"),
        "output": use("output"),
        "cache_read": use("cache_read"),
        "reasoning": use("reasoning"),
        "busy_s": round(sum(t.get("duration_ms", 0) for t in turns) / 1000, 1),
        "first": times[0] if times else "",
        "last": times[-1] if times else "",
        "calls": dict(calls),
        "turn_s": [round(t.get("duration_ms", 0) / 1000) for t in turns],
        "turn_steps": [t.get("steps", 0) for t in turns],
    }


results = {}
for run in sorted(glob.glob(E + "/runs/*-*")):
    name = os.path.basename(run)
    if name.startswith("pilot"):
        continue
    r = {"name": name}
    try:
        code, wall = open(run + "/wall.txt").read().split()
        r["exit"], r["wall_s"] = int(code), round(float(wall), 1)
    except Exception:
        r["exit"], r["wall_s"] = None, None
    try:
        env = json.loads(open(run + "/out.json").read().strip().splitlines()[-1])
    except Exception:
        env = {}
    r["answer"] = env.get("answer") or ""
    r["error"] = env.get("error") or ""
    stop = env.get("stop", {})
    r["root"] = {
        "reason": stop.get("reason"),
        "detail": stop.get("detail", ""),
        "steps": stop.get("steps"),
        "tool_calls": stop.get("tool_calls"),
        **{
            k: env.get("usage", {}).get(k, 0)
            for k in ("input", "output", "cache_read", "reasoning")
        },
    }
    r["routes"] = env.get("routes")
    r["trace_tools"] = dict(
        collections.Counter(t.get("name", "?") for t in env.get("trace", []) if isinstance(t, dict))
    )
    r["sessions"] = [
        session(p) for p in sorted(glob.glob(run + "/home/.uagent/history/*/*.events.jsonl"))
    ]
    debug, bad = lines(run + "/debug.jsonl")
    ev = collections.Counter(e.get("event") for e in debug)
    faults = collections.Counter(
        e.get("data", {}).get("fault") for e in debug if e.get("event") == "model_fault"
    )
    r["debug"] = {
        "events": len(debug),
        "unparsable_lines": bad,
        "model_requests": ev.get("model_request", 0),
        "mail_delivered": ev.get("mail_delivered", 0),
        "steering": ev.get("steering_applied", 0),
        "faults": dict(faults),
        "rejection_loops": ev.get("deterministic_rejection_loop", 0),
        "retries": ev.get("model_retry", 0) + ev.get("http_retry", 0),
        "kinds": dict(ev),
    }
    r["mail_files"] = sum(len(f) for _, _, f in os.walk(run + "/home/.uagent/mail"))
    for extra in ("tests.txt", "changed.txt"):
        if os.path.exists(run + "/" + extra):
            r[extra] = open(run + "/" + extra).read()[-600:]
    r["worktrees"] = len(glob.glob(run + "/home/.uagent/worktrees/*/*"))
    results[name] = r

json.dump(results, open(E + "/results.json", "w"), indent=1)
fmt = "{:16} {:>5} {:>6} {:>10} {:>4} {:>5} {:>9} {:>7} {:>8}  {}"
print(
    fmt.format(
        "run", "exit", "wall", "reason", "sess", "steps", "input", "output", "cached", "sessions"
    )
)
for name, r in results.items():
    s = r["sessions"]
    # The root of a solo/sub run is not saved as a session; a coordinator's is.
    extra_root = [] if name.endswith("coord") else [r["root"]]
    rows = s + extra_root

    def tot(k, rows=rows):
        return sum(x.get(k) or 0 for x in rows)

    print(
        fmt.format(
            name,
            str(r["exit"]),
            str(r["wall_s"]),
            str(r["root"]["reason"]),
            len(s) + len(extra_root),
            tot("steps"),
            tot("input"),
            tot("output"),
            tot("cache_read"),
            " ".join(f"{x['kind']}:{x['steps']}st/{x['input'] // 1000}k" for x in s),
        )
    )
