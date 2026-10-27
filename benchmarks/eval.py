#!/usr/bin/env python3
"""Scenario-driven behavioral evaluation for µAgent.

A scenario is one declarative JSON file under `benchmarks/scenarios/`: the
workspace fixture, the prompt, a scripted provider, and the checks that define a
good run. Scores and round counts are compared against committed baselines, so a
change that makes the agent spend more rounds, drop a batch, stop deduplicating
or lose its answer fails a gate instead of being argued about.

The suite scripts the provider, so it measures *harness* behavior — the part
this repository owns — not model quality.

    python3 benchmarks/eval.py build/debug/uagent --check
    python3 benchmarks/eval.py build/debug/uagent --update
    python3 benchmarks/eval.py build/debug/uagent --scenario CASE
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
SCENARIO_DIR = ROOT / "benchmarks" / "scenarios"
BASELINE_PATH = ROOT / "benchmarks" / "baselines" / "hermetic.json"
# The route name every scripted case is run against.
MODEL = "eval"

# One HTTP/SSE fixture serves the integration suite and this harness; a second
# copy would drift from the transport the tests actually exercise.
sys.path.insert(0, str(ROOT / "tests"))

# isort: off
from integration_support import Server, event, write_session  # noqa: E402
from session_support import remove_runtime, stop_sessions  # noqa: E402
from run_trace import (  # noqa: E402
    measured_command,
    peak_rss,
    read_trace,
)
from run_trace import trace_metrics as run_trace_metrics  # noqa: E402
from session_metrics import provenance_cohort, safe_provenance  # noqa: E402

# isort: on


def trace_metrics(records: list[dict[str, Any]]) -> dict[str, Any]:
    """Trajectory aggregates with this harness's provenance redaction applied."""
    return run_trace_metrics(records, provenance_fn=safe_provenance, cohort_fn=provenance_cohort)


# --- scenarios ---------------------------------------------------------------


def validate_predicates(when: Any) -> None:
    if not isinstance(when, dict):
        raise ValueError("scenario predicates must be an object")
    for key, value in when.items():
        if key == "no_tools":
            valid = isinstance(value, bool)
        elif key in ("tool_results", "min_tool_results", "max_tool_results"):
            valid = type(value) is int and value >= 0
        elif key == "last_starts_with":
            valid = isinstance(value, str)
        elif key in ("contains", "absent"):
            valid = isinstance(value, str) or (
                isinstance(value, list) and all(isinstance(v, str) for v in value)
            )
        else:
            raise ValueError(f"unknown scenario predicate: {key}")
        if not valid:
            raise ValueError(f"invalid scenario predicate {key}: {value!r}")


def load_scenarios(selected: list[str]) -> list[dict[str, Any]]:
    scenarios = []
    for path in sorted(SCENARIO_DIR.glob("*.json")):
        scenario = json.loads(path.read_text(encoding="utf-8"))
        scenario.setdefault("name", path.stem)
        if selected and scenario["name"] not in selected:
            continue
        for rule in scenario.get("script", []):
            validate_predicates(rule.get("when", {}))
        scenarios.append(scenario)
    unknown = sorted(set(selected) - {scenario["name"] for scenario in scenarios})
    if unknown:
        raise SystemExit(f"unknown scenario: {unknown}")
    if not scenarios:
        raise SystemExit(f"no scenarios found in {SCENARIO_DIR}")
    return scenarios


def file_contents(spec: Any) -> str:
    """A fixture file is a string, or a body plus a repeated padding line."""
    if isinstance(spec, str):
        return spec
    text = str(spec.get("text", ""))
    template = spec.get("repeat_text")
    if template:
        text += "".join(str(template).format(index=index) for index in range(int(spec["repeat"])))
    return text


def materialize(workspace: Path, files: dict[str, Any]) -> None:
    for name, spec in files.items():
        path = workspace / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(file_contents(spec), encoding="utf-8")


def snapshot(root: Path) -> dict[str, str]:
    return {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file() and ".uagent" not in path.parts
    }


# --- scripted provider -------------------------------------------------------


class Script:
    """Answer each request with the first rule whose `when` clause matches.

    A request no rule claims is a scenario bug, not a passing run: it is
    recorded and reported rather than silently answered.
    """

    def __init__(self, rules: list[dict[str, Any]], capture: dict[str, str]) -> None:
        for rule in rules:
            validate_predicates(rule.get("when", {}))
        self.rules = rules
        # Some arguments only exist at run time - a process id, a session id -
        # so a scenario names a pattern and later calls refer to ${name}.
        self.capture = {name: re.compile(pattern) for name, pattern in capture.items()}
        self.captured: dict[str, str] = {}
        self.misses: list[str] = []
        self.bodies: list[dict[str, Any]] = []

    def __call__(self, _handler: Any, body: dict[str, Any]) -> dict[str, Any]:
        self.bodies.append(body)
        messages = body.get("messages") or []
        serialized = json.dumps(messages)
        results = sum(
            1 for item in messages if isinstance(item, dict) and item.get("role") == "tool"
        )
        last = str(messages[-1].get("content", "")) if messages else ""
        for name, pattern in self.capture.items():
            found = pattern.search(serialized)
            if found:
                self.captured[name] = found.group(1)
        for rule in self.rules:
            if self.matches(rule.get("when", {}), body, serialized, results, last):
                return self.payload(self.resolve(rule["respond"]))
        self.misses.append(last[:160])
        return event({"content": "SCRIPT-MISS"})

    def resolve(self, respond: dict[str, Any]) -> dict[str, Any]:
        def substitute(value: Any) -> Any:
            if not isinstance(value, str):
                return value
            for name, captured in self.captured.items():
                token = "${" + name + "}"
                if value == token:
                    return int(captured) if captured.isdigit() else captured
                value = value.replace(token, captured)
            return value

        resolved = json.loads(json.dumps(respond))
        for call in resolved.get("tool_calls", []):
            call["arguments"] = {
                key: substitute(value) for key, value in call.get("arguments", {}).items()
            }
        return resolved

    @staticmethod
    def matches(
        when: dict[str, Any], body: dict[str, Any], serialized: str, results: int, last: str
    ) -> bool:
        for key, value in when.items():
            if key == "no_tools" and bool(not body.get("tools")) != bool(value):
                return False
            if key == "tool_results" and results != int(value):
                return False
            if key == "min_tool_results" and results < int(value):
                return False
            if key == "max_tool_results" and results > int(value):
                return False
            if key == "last_starts_with" and not last.startswith(str(value)):
                return False
            if key == "contains" and not all(part in serialized for part in as_list(value)):
                return False
            if key == "absent" and any(part in serialized for part in as_list(value)):
                return False
        return True

    @staticmethod
    def payload(respond: dict[str, Any]) -> dict[str, Any]:
        usage = respond.get("usage")
        if "tool_calls" in respond:
            calls = [
                {
                    "index": index,
                    "id": call.get("id", f"call-{index}"),
                    "function": {
                        "name": call["name"],
                        "arguments": json.dumps(call.get("arguments", {})),
                    },
                }
                for index, call in enumerate(respond["tool_calls"])
            ]
            return event(
                {"tool_calls": calls},
                finish="tool_calls",
                usage=usage or {"prompt_tokens": 100, "completion_tokens": 8, "cost": 0.0001},
            )
        return event({"content": respond.get("content", "")}, usage=usage)


def as_list(value: Any) -> list[str]:
    return [str(value)] if isinstance(value, str) else [str(item) for item in value]


# --- running -----------------------------------------------------------------


def case_environment(scenario: dict[str, Any], variant: str, arguments, mock) -> dict[str, str]:
    # A scenario declares its inputs; it never inherits them. Run from inside a
    # live session the shell carries that session's route --
    # UAGENT_INTERNAL_WIRE_API alone makes the binary speak a dialect this
    # mock does not serve -- and every case fails for a reason the scenario
    # is not about. Names this function sets below are reapplied deliberately.
    env = {key: value for key, value in os.environ.items() if not key.startswith("UAGENT_")}
    env.update(
        {
            "UAGENT_MEMORY": "0",
            "UAGENT_MEMORY_GENERATE": "0",
            "UAGENT_MAX_TOKENS": "1200",
            "UAGENT_MAX_STEPS": "12",
            "UAGENT_MAX_TOOL_CALLS": "12",
            "UAGENT_MAX_TURN_SECONDS": str(arguments.timeout),
            "UAGENT_AUTO_COMPACT_PCT": "0",
            "UAGENT_INTERNAL_AUTO_COMPACT_TOKENS": "1" if variant == "compacted" else "0",
        }
    )
    env.update({key: str(value) for key, value in scenario.get("env", {}).items()})
    env.update(
        {key: str(value) for key, value in scenario.get("variant_env", {}).get(variant, {}).items()}
    )
    if arguments.prompt_overlay:
        env["UAGENT_INTERNAL_PROMPT_OVERLAY"] = str(Path(arguments.prompt_overlay).resolve())
    if arguments.toolset != "full":
        env["UAGENT_INTERNAL_TOOLSET"] = arguments.toolset
    else:
        env.pop("UAGENT_INTERNAL_TOOLSET", None)
    # Keep the loopback transport independent of runner-level proxy
    # configuration (notably macOS system libcurl behavior).
    for name in ("ALL_PROXY", "HTTP_PROXY", "HTTPS_PROXY"):
        env.pop(name, None)
        env.pop(name.lower(), None)
    env["NO_PROXY"] = "127.0.0.1,localhost"
    env["no_proxy"] = env["NO_PROXY"]
    env.update(
        {
            "UAGENT_BASE_URL": mock.url,
            "UAGENT_API_KEY": "evaluation-placeholder",
            "UAGENT_CONTEXT": "16384",
            "UAGENT_REQUEST_TIMEOUT": "10",
            "UAGENT_STREAM_TIMEOUT": "5",
        }
    )
    return env


def timeout_text(value: str | bytes | None) -> str:
    if value is None:
        return ""
    return value.decode(errors="replace") if isinstance(value, bytes) else value


def run_case(
    binary: Path,
    scenario: dict[str, Any],
    variant: str,
    arguments,
):
    with tempfile.TemporaryDirectory(prefix="uagent-eval-") as temp:
        root = Path(temp)
        workspace = root / "workspace"
        home = root / "home"
        workspace.mkdir()
        home.mkdir()
        materialize(workspace, scenario.get("files", {}))
        before = snapshot(workspace)
        trace_path = root / "trace.jsonl"

        script = Script(scenario.get("script", []), scenario.get("capture", {}))
        mock = Server([script])
        # Saved conversations the folder already has, for a panel to read.
        for saved in scenario.get("sessions", []):
            write_session(
                home, saved["name"], saved["messages"], cwd=workspace, title=saved["title"]
            )
        try:
            env = case_environment(scenario, variant, arguments, mock)
            env["HOME"] = str(home)
            # Beside HOME rather than above it: the runner's own TMPDIR is an
            # ancestor of this case's ~/.uagent, and the sandbox will not grant
            # a writable root that contains the agent's own state.
            scratch = root / "tmp"
            scratch.mkdir(exist_ok=True)
            env["TMPDIR"] = str(scratch)
            prompt = str(scenario["prompt"])
            prompt = prompt.replace("${workspace}", str(workspace))
            prompt = prompt.replace("${workspace_uri}", workspace.as_uri())
            cli = [
                # A panel scenario talks to the folder's panel.
                *(["panel"] if scenario.get("panel") else []),
                "--json",
                "--no-memory",
                f"--debug={trace_path}",
                *(["--yolo"] if scenario.get("yolo") else []),
                "--model",
                MODEL,
                "--budget",
                "0.10",
                "-p",
                prompt,
            ]
            process_timeout = arguments.timeout + 15
            started = time.monotonic()
            try:
                process = subprocess.run(
                    measured_command(binary, cli),
                    cwd=workspace,
                    env=env,
                    stdin=subprocess.DEVNULL,
                    text=True,
                    capture_output=True,
                    timeout=process_timeout,
                    check=False,
                )
            except subprocess.TimeoutExpired as error:
                process = subprocess.CompletedProcess(
                    error.cmd,
                    124,
                    stdout=timeout_text(error.stdout),
                    stderr=timeout_text(error.stderr)
                    + f"\nhard session timeout after {process_timeout}s",
                )
            elapsed = time.monotonic() - started
        finally:
            # Threads a panel started outlive its answer; none may
            # outlive the case.
            stop_sessions(home)
            remove_runtime(home)
            mock.close()

        try:
            envelope = json.loads(process.stdout)
        except json.JSONDecodeError:
            envelope = {}
        metrics = trace_metrics(read_trace(trace_path))
        answer = str(envelope.get("answer") or "")
        bodies = script.bodies
        checks = evaluate(
            scenario,
            variant,
            answer=answer,
            metrics=metrics,
            unchanged=snapshot(workspace) == before,
            returncode=process.returncode,
            bodies=bodies,
            workspace=workspace,
        )
        result = {
            "scenario": scenario["name"],
            # A capability scenario is a hill to climb and does not gate the
            # build; it graduates into the regression tier once it holds green.
            "tier": scenario.get("tier", "regression"),
            "variant": variant,
            "cohort": metrics["cohort"],
            "provenance": metrics["provenance"],
            "elapsed_seconds": round(elapsed, 3),
            "peak_rss_bytes": peak_rss(process.stderr),
            "usage": envelope.get("usage", {}),
            "model_requests": metrics["model_requests"],
            "tool_calls": metrics["tool_calls"],
            "max_batch": metrics["max_batch"],
            "no_action_rounds": metrics["no_action_rounds"],
            "compactions": metrics["compactions"],
            "cumulative_estimated_context_bytes": metrics["cumulative_estimated_context_bytes"],
            "estimated_context_bytes_progression": metrics["estimated_context_bytes_progression"],
            "max_estimated_context_bytes": metrics["max_estimated_context_bytes"],
            "initial_schema_bytes": metrics["initial_schema_bytes"],
            "tool_result_chars": metrics["tool_result_chars"],
            "model_duration_ms": round(metrics["model_duration_ms"], 3),
            "request_preparation_ms": round(metrics["request_preparation_ms"], 3),
            "input_tokens": int(metrics["usage"]["input"]),
            "cache_read_tokens": int(metrics["usage"]["cache_read"]),
            "issue_codes": dict(metrics["issue_codes"]),
            "failed_calls": metrics["failed_calls"],
            "failed_call_recoveries": metrics["failed_call_recoveries"],
            "unrecovered_failed_calls": metrics["unrecovered_failed_calls"],
            "browser_commands": metrics["browser_commands"],
            "browser_snapshot_chars": metrics["browser_snapshot_chars"],
            "checks": checks,
            "score": sum(1 for value in checks.values() if value),
            "checks_total": len(checks),
            "passed": all(checks.values()),
            # The failure-category vector says what broke, which a pass rate
            # alone never does.
            "failures": [name for name, ok in checks.items() if not ok],
            "bytes_per_check": round(
                metrics["cumulative_estimated_context_bytes"]
                / max(sum(1 for value in checks.values() if value), 1)
            ),
            "answer": answer,
            "script_misses": script.misses,
            "error": envelope.get("error")
            or (process.stderr.strip()[-400:] if process.returncode else None),
        }
        scoring = scenario.get("scoring", {})
        if scoring:
            outcome_checks = as_list(scoring.get("outcome_checks", ["process_ok"]))
            outcome = all(checks.get(name, False) for name in outcome_checks)
            target_rounds = max(1, int(scoring.get("target_rounds", 1)))
            rounds = max(1, metrics["model_requests"])
            result["experiment"] = {
                "outcome": outcome,
                "rounds": metrics["model_requests"],
                "round_adjusted_score": round(
                    (min(1.0, target_rounds / rounds) if outcome else 0.0), 4
                ),
                "cumulative_context_bytes": metrics["cumulative_estimated_context_bytes"],
                "snapshot_chars": metrics["browser_snapshot_chars"],
                "recovered_failures": metrics["failed_call_recoveries"],
            }
        return result


# --- checks ------------------------------------------------------------------


def answer_shape(answer: str, labels: list[str]) -> bool:
    """Accept either labelled headings/bold or one bullet per label."""
    lines = answer.splitlines()
    bullets = [index for index, line in enumerate(lines) if line.startswith("- ")]
    preamble = lines[: bullets[0]] if bullets else lines
    headings = all(
        re.search(rf"(?im)^\s*(?:#{{1,6}}\s+{label}|\*\*{label}\b)", answer) for label in labels
    )
    return headings or (
        len(bullets) == len(labels) and sum(len(line.strip()) for line in preamble) <= 80
    )


def tool_result_text(bodies: list[dict[str, Any]]) -> str:
    """Everything the harness handed back to the model in the final request."""
    if not bodies:
        return ""
    return "\n".join(
        str(message.get("content", ""))
        for message in bodies[-1].get("messages", [])
        if isinstance(message, dict) and message.get("role") == "tool"
    )


def evaluate(scenario, variant, *, answer, metrics, unchanged, returncode, bodies, workspace):
    wanted = scenario.get("checks", {})
    checks = {"process_ok": returncode == 0}
    read_paths = {
        str(call["arguments"].get("path", ""))
        for call in metrics["calls"]
        if call["name"] == "read_path" and isinstance(call["arguments"], dict)
    }
    for name, value in wanted.items():
        if name == "workspace_unchanged":
            checks[name] = unchanged == bool(value)
        elif name == "read_paths":
            checks[name] = set(as_list(value)) <= read_paths
        elif name == "answer_contains":
            checks[name] = all(part in answer for part in as_list(value))
        elif name == "answer_shape":
            checks[name] = answer_shape(answer, as_list(value))
        elif name == "forbidden_tools":
            used = {call["name"] for call in metrics["calls"]}
            checks[name] = not (used & set(as_list(value)))
        elif name == "max_requests":
            checks[name] = metrics["model_requests"] <= int(value)
        elif name == "max_tool_calls":
            checks[name] = metrics["tool_calls"] <= int(value)
        elif name == "min_batch":
            checks[name] = metrics["max_batch"] >= int(value)
        elif name == "min_cumulative_context_bytes":
            checks[name] = metrics["cumulative_estimated_context_bytes"] >= int(value)
        elif name == "min_tool_result_chars":
            checks[name] = metrics["tool_result_chars"] >= int(value)
        elif name == "min_recovered_failures":
            checks[name] = metrics["failed_call_recoveries"] >= int(value)
        elif name == "min_argument_issues":
            checks[name] = sum(metrics["issue_codes"].values()) >= int(value)
        elif name == "argument_issue_codes":
            checks[name] = set(as_list(value)) <= set(metrics["issue_codes"])
        elif name == "min_browser_commands":
            checks[name] = metrics["browser_commands"] >= int(value)
        elif name == "min_browser_snapshot_chars":
            checks[name] = metrics["browser_snapshot_chars"] >= int(value)
        elif name == "recovery_markers":
            positions = [metrics["tool_result_text"].find(part) for part in as_list(value)]
            checks[name] = all(position >= 0 for position in positions) and positions == sorted(
                positions
            )
        elif name == "events":
            checks[name] = all(metrics["events"][key] >= int(count) for key, count in value.items())
        elif name in ("request_ordered", "request_absent_repeats"):
            # Request-body evidence exists only behind the scripted provider.
            if bodies:
                serialized = json.dumps(bodies[-1].get("messages", []))
                if name == "request_ordered":
                    positions = [serialized.find(part) for part in as_list(value)]
                    checks[name] = all(pos >= 0 for pos in positions) and positions == sorted(
                        positions
                    )
                else:
                    checks[name] = all(
                        serialized.count(key) <= int(limit) for key, limit in value.items()
                    )
        elif name == "files_after":
            checks[name] = all(
                (workspace / path).is_file()
                and (workspace / path).read_text(encoding="utf-8") == content
                for path, content in value.items()
            )
        elif name == "no_repeated_calls":
            signatures = [
                (call["name"], json.dumps(call["arguments"], sort_keys=True))
                for call in metrics["calls"]
            ]
            checks[name] = (len(set(signatures)) == len(signatures)) == bool(value)
        elif name == "result_contains":
            # Tool results are only observable behind the scripted provider.
            if bodies:
                results = tool_result_text(bodies)
                checks[name] = all(part in results for part in as_list(value))
        else:
            raise SystemExit(f"{scenario['name']}: unknown check '{name}'")
    if variant == "compacted":
        checks["compaction_mode"] = metrics["compactions"] > 0
    elif "compacted" in scenario.get("variants", []):
        checks["compaction_mode"] = metrics["compactions"] == 0
    return checks


# --- baselines ---------------------------------------------------------------


def baseline_key(result: dict[str, Any]) -> str:
    return f"{result['scenario']}/{result['variant']}"


def load_baseline() -> dict[str, Any]:
    if not BASELINE_PATH.exists():
        return {"scenarios": {}}
    baseline = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    if baseline.get("schema") != "uagent.eval.baseline.v2":
        raise ValueError("missing or unsupported evaluation baseline schema")
    return baseline


def compare(results: list[dict[str, Any]], baseline: dict[str, Any]) -> list[dict[str, Any]]:
    recorded = baseline.get("scenarios", {})
    comparisons = []
    for result in results:
        key = baseline_key(result)
        base = recorded.get(key)
        regressions = []
        if not result["passed"] and result["tier"] == "regression":
            regressions.append("failed checks: " + ", ".join(result["failures"]))
        if result["script_misses"]:
            regressions.append(f"{len(result['script_misses'])} unscripted requests")
        if base is None:
            comparisons.append(
                {"key": key, "status": "new", "regressions": regressions, "passed": not regressions}
            )
            continue
        if result["score"] < base["score"]:
            regressions.append(f"score {base['score']} → {result['score']}")
        for field in ("model_requests", "tool_calls"):
            if result[field] > base[field]:
                regressions.append(f"{field} {base[field]} → {result[field]}")
        if result["max_batch"] < base["max_batch"]:
            regressions.append(f"max_batch {base['max_batch']} → {result['max_batch']}")
        # Tool schemas are sent on every request, so their growth is gated on its
        # own rather than hidden inside the context allowance below.
        schema_ceiling = int(base.get("initial_schema_bytes", 0) * 1.05) + 16
        if base.get("initial_schema_bytes") and result["initial_schema_bytes"] > schema_ceiling:
            regressions.append(
                f"schema bytes {base['initial_schema_bytes']} → "
                f"{result['initial_schema_bytes']} (>5%)"
            )
        ceiling = int(base["max_estimated_context_bytes"] * 1.05) + 512
        if result["max_estimated_context_bytes"] > ceiling:
            regressions.append(
                f"context bytes {base['max_estimated_context_bytes']} → "
                f"{result['max_estimated_context_bytes']} (>5%)"
            )
        comparisons.append(
            {
                "key": key,
                "status": "regression" if regressions else "ok",
                "regressions": regressions,
                "passed": not regressions,
            }
        )
    return comparisons


def variant_comparisons(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Forced compaction must not cost quality."""
    by_case = {(result["scenario"], result["variant"]): result for result in results}
    comparisons = []
    for (scenario, variant), result in by_case.items():
        control = by_case.get((scenario, "control"))
        if variant != "compacted" or control is None:
            continue
        regressions = []
        if result["score"] < control["score"]:
            regressions.append(f"compacted score {control['score']} → {result['score']}")
        comparisons.append(
            {
                "key": f"{scenario}/compacted-vs-control",
                "status": "regression" if regressions else "ok",
                "regressions": regressions,
                "passed": not regressions,
            }
        )
    return comparisons


def write_baseline(results: list[dict[str, Any]]) -> None:
    payload = {
        "schema": "uagent.eval.baseline.v2",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "note": "Regenerate with `python3 benchmarks/eval.py BINARY --update` and review the diff.",
        "scenarios": {
            baseline_key(result): {
                "score": result["score"],
                "checks_total": result["checks_total"],
                "model_requests": result["model_requests"],
                "tool_calls": result["tool_calls"],
                "max_batch": result["max_batch"],
                "no_action_rounds": result["no_action_rounds"],
                "compactions": result["compactions"],
                "max_estimated_context_bytes": result["max_estimated_context_bytes"],
                "cumulative_estimated_context_bytes": result["cumulative_estimated_context_bytes"],
                "tool_result_chars": result["tool_result_chars"],
                "initial_schema_bytes": result["initial_schema_bytes"],
            }
            for result in sorted(results, key=baseline_key)
        },
    }
    BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
    BASELINE_PATH.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"baseline written: {BASELINE_PATH.relative_to(ROOT)} ({len(results)} cases)")


def eval_self_test() -> int:
    browser = trace_metrics(
        [
            {
                "event": "tool_call",
                "data": {
                    "occurrence_id": "response-1:first",
                    "name": "run",
                    "arguments": {"command": "playwright-cli first"},
                },
            },
            {
                "event": "tool_result",
                "data": {
                    "occurrence_id": "response-1:first",
                    "name": "run",
                    "status": "error",
                    "result_chars": 7,
                },
            },
            {
                "event": "tool_call",
                "data": {
                    "occurrence_id": "response-2:retry",
                    "name": "run",
                    "arguments": {"command": "playwright-cli recover"},
                },
            },
            {
                "event": "tool_result",
                "data": {
                    "occurrence_id": "response-2:retry",
                    "name": "run",
                    "status": "success",
                    "result_chars": 11,
                },
            },
        ]
    )
    if browser["browser_commands"] != 2 or browser["browser_snapshot_chars"] != 18:
        raise AssertionError("browser calls are not correlated by occurrence identity")
    for invalid in (
        {"tool_result": 999},
        {"no_tools": 1},
        {"tool_results": True},
        {"contains": [2]},
    ):
        try:
            Script([{"when": invalid, "respond": {}}], {})
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted invalid predicate: {invalid}")
    return 0


# --- reporting ---------------------------------------------------------------


def print_results(results, comparisons):
    print(
        f"{'scenario/variant':<38} {'score':>7} {'req':>4} {'idle':>5} {'tools':>6} "
        f"{'batch':>5} {'maxctx':>7} {'cumctx':>8} {'results':>8} {'wall':>7}"
    )
    for result in results:
        print(
            f"{baseline_key(result):<38} "
            f"{result['score']}/{result['checks_total']:<5} "
            f"{result['model_requests']:>4} {result['no_action_rounds']:>5} "
            f"{result['tool_calls']:>6} "
            f"{result['max_batch']:>5} {result['max_estimated_context_bytes']:>7} "
            f"{result['cumulative_estimated_context_bytes']:>8} "
            f"{result['tool_result_chars']:>8} {result['elapsed_seconds']:>6.1f}s"
        )
        if result["failures"]:
            print(f"    failed: {', '.join(result['failures'])} [{result['tier']}]")
        if result["error"]:
            print(f"    error: {result['error']}")
    for comparison in comparisons:
        if comparison["status"] == "ok":
            continue
        detail = "; ".join(comparison["regressions"]) or "no baseline recorded"
        print(f"{comparison['status'].upper()}: {comparison['key']}: {detail}")
    for note in graduation_notes(results):
        print(f"GRADUATE: {note}")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, help="uagent binary to evaluate")
    parser.add_argument("--scenario", action="append", default=[], help="name; repeatable")
    parser.add_argument("--variant", action="append", default=[], help="name; repeatable")
    parser.add_argument("--check", action="store_true", help="fail on baseline regressions")
    parser.add_argument("--update", action="store_true", help="rewrite the committed baseline")
    parser.add_argument("--report", type=Path, help="write the full JSON report")
    parser.add_argument("--prompt-overlay", type=Path, help="prompt overlay JSON for every run")
    parser.add_argument("--timeout", type=int, default=120, help="seconds per run")
    parser.add_argument("--toolset", choices=("full", "lean"), default="full")
    parser.add_argument("--self-test", action="store_true", help="validate the harness helpers")
    arguments = parser.parse_args()
    if arguments.self_test:
        return arguments
    if arguments.binary is None:
        parser.error("binary is required unless --self-test is used")
    arguments.binary = arguments.binary.resolve()
    if not arguments.binary.is_file():
        parser.error(f"binary does not exist: {arguments.binary}")
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    return arguments


def graduation_notes(results: list[dict[str, Any]]) -> list[str]:
    """A capability scenario that holds green belongs in the regression tier."""
    return [
        f"{baseline_key(result)}: capability scenario is green — set "
        f'"tier": "regression" to gate it'
        for result in results
        if result["tier"] == "capability" and result["passed"]
    ]


def main() -> int:
    arguments = parse_args()
    if arguments.self_test:
        return eval_self_test()
    jobs = []
    for scenario in load_scenarios(arguments.scenario):
        variants = scenario.get("variants", ["control"])
        if arguments.variant:
            variants = [name for name in variants if name in arguments.variant]
        jobs.extend((scenario, variant) for variant in variants)
    if not jobs:
        raise SystemExit("no scenario variants selected")

    results = [
        run_case(arguments.binary, scenario, variant, arguments) for scenario, variant in jobs
    ]
    comparisons = variant_comparisons(results) + compare(results, load_baseline())
    print_results(results, comparisons)
    if arguments.update:
        write_baseline(results)
    report = {
        "schema": "uagent.eval.v2",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "binary": {
            "path": str(arguments.binary),
            "bytes": arguments.binary.stat().st_size,
            "sha256": hashlib.sha256(arguments.binary.read_bytes()).hexdigest(),
        },
        "toolset": arguments.toolset,
        "prompt_overlay": str(arguments.prompt_overlay) if arguments.prompt_overlay else None,
        "results": results,
        "comparisons": comparisons,
    }
    if arguments.report:
        arguments.report.parent.mkdir(parents=True, exist_ok=True)
        arguments.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not all(result["passed"] for result in results if result["tier"] == "regression"):
        return 1
    if arguments.check and not all(item["passed"] for item in comparisons):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
