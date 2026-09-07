#!/usr/bin/env python3
"""A scripted stand-in for the µAgent binary, used by the controller tests.

It speaks the part of the CLI the controller depends on (`--debug`, `--model`,
`--budget`, `-p`), applies a plan carried in its own bundle, and emits the JSON
envelope and trace events the controller measures. Behaviour therefore travels
with the bundle exactly as prompts and skills do for the real binary, so a
control and a candidate differ only in the version that was launched.
"""

import hashlib
import json
import os
import pathlib
import sys
import time


def parse(argv):
    options = {"trace": None, "prompt": "", "budget": None, "model": ""}
    for index, argument in enumerate(argv):
        if argument.startswith("--debug="):
            options["trace"] = pathlib.Path(argument.split("=", 1)[1])
        elif argument == "--model" and index + 1 < len(argv):
            options["model"] = argv[index + 1]
        elif argument == "--budget" and index + 1 < len(argv):
            options["budget"] = argv[index + 1]
        elif argument == "-p" and index + 1 < len(argv):
            options["prompt"] = argv[index + 1]
    return options


def load_plan():
    directory = os.environ.get("UAGENT_SKILL_PATH")
    if not directory:
        return {}
    path = pathlib.Path(directory) / "plan.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}


def emit(trace, plan, options):
    if trace is None:
        return
    usage = plan.get("usage") or {}
    records = [{"event": "session_ready", "data": {"route": options["model"]}}]
    for _ in range(int(plan.get("model_requests", 1))):
        records.append({"event": "model_request", "data": {"message_chars": 400}})
    for index in range(int(plan.get("tool_calls", 0))):
        failed = index < int(plan.get("tool_failures", 0))
        identifier = f"call-{index}"
        records.append(
            {
                "event": "tool_call",
                "data": {"id": identifier, "turn": 1, "name": "run", "arguments": {}},
            }
        )
        records.append(
            {
                "event": "tool_result",
                "data": {
                    "id": identifier,
                    "turn": 1,
                    "name": "run",
                    "status": "error" if failed else "ok",
                    "result_chars": 32,
                },
            }
        )
    records.append({"event": "turn_end", "data": {"usage": usage}})
    trace.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")


def apply_plan(plan, workspace):
    prior = workspace / "src/improvement.py"
    value = int(prior.read_text().split("=")[-1]) if prior.exists() else 0
    for relative, body in (plan.get("writes") or {}).items():
        if relative == "src/improvement.py" and value:
            body = f"VALUE = {value + 1}\n"
        target = workspace / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(body, encoding="utf-8")
    for relative in plan.get("deletes") or []:
        target = workspace / relative
        if target.is_file():
            target.unlink()
    claim = plan.get("claim")
    if claim:
        if claim["verify_command"] == "measure-increment":
            claim["verify_command"] = (
                'python3 -c \'from pathlib import Path; p=Path("src/improvement.py"); '
                f'assert p.exists() and int(p.read_text().split("=")[-1]) > {value}\''
            )
        (workspace / ".uagent-improvement.json").write_text(
            json.dumps({"schema": "uagent.improvement.claim.v1", **claim}), encoding="utf-8"
        )


def main():
    options = parse(sys.argv[1:])
    plan = load_plan()
    home = pathlib.Path(os.environ.get("HOME", "."))
    # Written outside the subject tree: the tests read these to prove both sides
    # of a pair received identical inputs and that the recorded bundle ran.
    (home / "prompt.sha256").write_text(
        hashlib.sha256(options["prompt"].encode()).hexdigest(), encoding="utf-8"
    )
    (home / "session.json").write_text(
        json.dumps(
            {
                "executable": os.path.realpath(sys.argv[0]),
                "skills": os.environ.get("UAGENT_SKILL_PATH", ""),
                "model": options["model"],
                "budget": options["budget"],
                "cwd": os.getcwd(),
                "limits": {
                    name: value
                    for name, value in os.environ.items()
                    if name.startswith("UAGENT_MAX") or name == "UAGENT_SESSION_BUDGET"
                },
            },
            sort_keys=True,
        ),
        encoding="utf-8",
    )
    apply_plan(plan, pathlib.Path.cwd())
    emit(options["trace"], plan, options)
    time.sleep(float(plan.get("sleep_seconds", 0)))
    print(json.dumps({"answer": plan.get("answer", "done"), "usage": plan.get("usage") or {}}))
    return int(plan.get("exit", 0))


if __name__ == "__main__":
    raise SystemExit(main())
