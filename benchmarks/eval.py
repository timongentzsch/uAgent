#!/usr/bin/env python3
"""Scenario-driven behavioral evaluation for µAgent.

A scenario is one declarative JSON file under `benchmarks/scenarios/`: the
workspace fixture, the prompt, a scripted provider, and the checks that define a
good run. Scores and round counts are compared against committed baselines, so a
change that makes the agent spend more rounds, drop a batch, stop deduplicating
or lose its answer fails a gate instead of being argued about.

The hermetic suite scripts the provider, so it measures *harness* behavior — the
part this repository owns — not model quality. `--run --model` replays the
same scenarios against a real route as the periodic reality check. Live runs
require explicit route authority: either reported cost with a hard USD budget,
or an operator-declared non-billable cheap route with hard session, model-call,
tool-call, output-token, and wall-clock limits. Cheapness is never inferred from
a model name.

    python3 benchmarks/eval.py build/debug/uagent --check
    python3 benchmarks/eval.py build/debug/uagent --update
    python3 benchmarks/eval.py build/debug/uagent --scenario CASE --trials 5
    python3 benchmarks/eval.py build/release/uagent --run --model provider/model \
        --scenario CASE --trials 5 --cost-authority authority.json
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
SCENARIO_DIR = ROOT / "benchmarks" / "scenarios"
BASELINE_PATH = ROOT / "benchmarks" / "baselines" / "hermetic.json"
SELF_TEST_PATH = ROOT / "tests" / "fixtures" / "eval" / "alternating_trials.json"
CHEAP_AUTHORITY_SELF_TEST_PATH = ROOT / "tests" / "fixtures" / "eval" / "cheap_authority.json"

# One HTTP/SSE fixture serves the integration suite and this harness; a second
# copy would drift from the transport the tests actually exercise.
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "skills" / "self-improve" / "scripts"))

# isort: off
from integration_support import Server, event  # noqa: E402
from live_authority import (  # noqa: E402
    AuthorityError,
    load_authority,
    normalize_route_authority,
)
from session_metrics import provenance_cohort, safe_provenance  # noqa: E402

# isort: on


# --- scenarios ---------------------------------------------------------------


def load_scenarios(selected: list[str]) -> list[dict[str, Any]]:
    scenarios = []
    for path in sorted(SCENARIO_DIR.glob("*.json")):
        scenario = json.loads(path.read_text(encoding="utf-8"))
        scenario.setdefault("name", path.stem)
        if selected and scenario["name"] not in selected:
            continue
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


def copy_user_config(home: Path) -> None:
    source = Path.home() / ".uagent" / ".config"
    if source.is_file():
        target = home / ".uagent" / ".config"
        target.parent.mkdir(parents=True)
        shutil.copyfile(source, target)
        target.chmod(0o600)


def read_trace(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    records = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(record, dict):
            records.append(record)
    return records


def trace_metrics(records: list[dict[str, Any]]) -> dict[str, Any]:
    """Reconstruct per-request context and privacy-safe trajectory aggregates."""
    events: collections.Counter[str] = collections.Counter()
    batches: list[int] = []
    calls: list[dict[str, Any]] = []
    request_chars: list[int] = []
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
            provenance = safe_provenance(data.get("provenance"))
            route = str(data.get("route") or "")
        elif name == "model_request":
            first = first or data
            batches.append(0)
            if data.get("projected_context"):
                message_chars = int(data.get("message_chars") or 0)
            elif "message_chars" in data:
                current_messages = int(data.get("message_chars") or 0)
                message_chars = current_messages
            else:
                current_messages += int(data.get("new_message_chars") or 0)
                message_chars = current_messages
            schema_chars = int(data.get("schema_chars") or 0) if data.get("native_tools") else 0
            request_chars.append(message_chars + schema_chars)
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
                "id": str(data.get("id") or ""),
                "turn": data.get("turn"),
                "step": data.get("step"),
                "name": data.get("name"),
                "arguments": arguments,
            }
            calls.append(call)
            if call["name"] == "run" and isinstance(arguments, dict):
                if "playwright-cli" in str(arguments.get("command", "")):
                    browser_calls.add(call["id"])
            if batches:
                batches[-1] += 1
        elif name == "tool_result":
            chars = int(data.get("result_chars") or 0)
            tool_result_texts.append(str(data.get("result") or ""))
            tool_name = str(data.get("name") or "?")
            tool_result_chars += chars
            result_chars_by_tool[tool_name] += chars
            if str(data.get("id") or "") in browser_calls:
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
                for field in (
                    "input",
                    "output",
                    "cache_read",
                    "cache_write",
                    "reasoning",
                    "web_searches",
                ):
                    usage[field] += int(turn_usage.get(field) or 0)
                usage["cost"] += float(turn_usage.get("cost") or 0)
                if turn_usage.get("cost_reported"):
                    usage["cost_reported_turns"] += 1
        elif name == "compact_end" and data.get("outcome") == "ok":
            compactions += 1
    no_action = max(len([count for count in batches[:-1] if count == 0]), 0)
    cohort = provenance_cohort(provenance)
    cumulative_request_chars = sum(request_chars)
    return {
        "model_requests": len(batches),
        "tool_calls": len(calls),
        "calls": calls,
        "max_batch": max(batches, default=0),
        "no_action_rounds": no_action,
        "compactions": compactions,
        "events": events,
        "estimated_request_chars": cumulative_request_chars,
        "cumulative_estimated_request_chars": cumulative_request_chars,
        "estimated_request_chars_progression": request_chars,
        "max_estimated_request_chars": max(request_chars, default=0),
        "initial_schema_chars": int(first.get("schema_chars") or 0),
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
        "cohort": cohort,
        "route": route,
    }


def case_environment(scenario: dict[str, Any], variant: str, arguments, mock) -> dict[str, str]:
    env = dict(os.environ)
    if mock is not None:
        # A mocked scenario declares its inputs; it never inherits them. Run
        # from inside a live session the shell carries that session's route --
        # UAGENT_WIRE_API alone makes the binary speak a dialect this mock does
        # not serve -- and every case fails for a reason the scenario is not
        # about. Names this function sets below are reapplied deliberately.
        env = {key: value for key, value in env.items() if not key.startswith("UAGENT_")}
    env.update(
        {
            "UAGENT_MEMORY": "0",
            "UAGENT_MEMORY_GENERATE": "0",
            "UAGENT_MAX_TOKENS": "1200",
            "UAGENT_MAX_STEPS": "12",
            "UAGENT_MAX_TOOL_CALLS": "12",
            "UAGENT_MAX_TURN_SECONDS": str(arguments.timeout),
            "UAGENT_AUTO_COMPACT_PCT": "0",
            "UAGENT_AUTO_COMPACT_TOKENS": "1" if variant == "compacted" else "0",
        }
    )
    env.update({key: str(value) for key, value in scenario.get("env", {}).items()})
    env.update(
        {key: str(value) for key, value in scenario.get("variant_env", {}).get(variant, {}).items()}
    )
    if arguments.prompt_overlay:
        env["UAGENT_PROMPT_OVERLAY"] = str(Path(arguments.prompt_overlay).resolve())
    if arguments.toolset != "full":
        env["UAGENT_TOOLSET"] = arguments.toolset
    else:
        env.pop("UAGENT_TOOLSET", None)
    if mock is not None:
        # Keep the hermetic loopback transport independent of runner-level proxy
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
                "UAGENT_FIRST_EVENT_TIMEOUT": "5",
                "UAGENT_STREAM_IDLE_TIMEOUT": "5",
            }
        )
        env.pop("UAGENT_PROVIDERS", None)
    return env


def timeout_text(value: str | bytes | None) -> str:
    if value is None:
        return ""
    return value.decode(errors="replace") if isinstance(value, bytes) else value


def run_case(
    binary: Path,
    scenario: dict[str, Any],
    variant: str,
    model: str,
    arguments,
    *,
    trial: int = 1,
    trial_seed: int = 0,
    budget: float | None = None,
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

        script = None
        mock = None
        if not arguments.run:
            script = Script(scenario.get("script", []), scenario.get("capture", {}))
            mock = Server([script])
        else:
            copy_user_config(home)
        try:
            env = case_environment(scenario, variant, arguments, mock)
            env["HOME"] = str(home)
            # Beside HOME rather than above it: the runner's own TMPDIR is an
            # ancestor of this case's ~/.uagent, and the sandbox will not grant
            # a writable root that contains the agent's own state.
            scratch = root / "tmp"
            scratch.mkdir(exist_ok=True)
            env["TMPDIR"] = str(scratch)
            authority = arguments.cost_authority_data["routes"][model] if arguments.run else None
            if authority is not None:
                apply_live_authority(env, authority)
            prompt = str(scenario["prompt"])
            prompt = prompt.replace("${workspace}", str(workspace))
            prompt = prompt.replace("${workspace_uri}", workspace.as_uri())
            cli = [
                "--json",
                "--no-memory",
                f"--debug={trace_path}",
                *(["--yolo"] if scenario.get("yolo") else []),
                "--model",
                model,
            ]
            if not arguments.run or authority["mode"] == "reported-cost":
                cli.extend(["--budget", str(arguments.max_cost if budget is None else budget)])
            cli.extend(["-p", prompt])
            process_timeout = arguments.timeout + 15
            if authority is not None and authority["mode"] == "non-billable-cheap":
                process_timeout = min(arguments.timeout, authority["limits"]["max_session_seconds"])
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
            if mock is not None:
                mock.close()

        try:
            envelope = json.loads(process.stdout)
        except json.JSONDecodeError:
            envelope = {}
        metrics = trace_metrics(read_trace(trace_path))
        answer = str(envelope.get("answer") or "")
        bodies = script.bodies if script else []
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
            "live_only": bool(scenario.get("live_only")),
            "variant": variant,
            "model": model,
            "authority_mode": authority["mode"] if authority is not None else "hermetic",
            "trial": trial,
            "trial_seed": trial_seed,
            "route": metrics["route"] if arguments.run else model,
            "cohort": metrics["cohort"],
            "provenance": metrics["provenance"],
            "elapsed_seconds": round(elapsed, 3),
            "peak_rss_bytes": peak_rss(process.stderr),
            "usage": envelope.get("usage", {}),
            "model_requests": metrics["model_requests"],
            "tool_calls": metrics["tool_calls"],
            "max_batch": metrics["max_batch"],
            "no_action_rounds": metrics["no_action_rounds"],
            # Which tools a live run reached for, which is the whole question
            # when the scenario does not script them.
            "tools_used": sorted({call["name"] for call in metrics["calls"]}),
            "compactions": metrics["compactions"],
            "estimated_request_chars": metrics["estimated_request_chars"],
            "cumulative_estimated_request_chars": metrics["cumulative_estimated_request_chars"],
            "estimated_request_chars_progression": metrics["estimated_request_chars_progression"],
            "max_estimated_request_chars": metrics["max_estimated_request_chars"],
            "initial_schema_chars": metrics["initial_schema_chars"],
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
            "chars_per_check": round(
                metrics["estimated_request_chars"]
                / max(sum(1 for value in checks.values() if value), 1)
            ),
            "answer": answer,
            "script_misses": script.misses if script else [],
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
                "cumulative_context_chars": metrics["cumulative_estimated_request_chars"],
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
        elif name == "min_cumulative_request_chars":
            checks[name] = metrics["cumulative_estimated_request_chars"] >= int(value)
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
    return json.loads(BASELINE_PATH.read_text(encoding="utf-8"))


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
        ceiling = int(base["max_estimated_request_chars"] * 1.05) + 512
        if result["max_estimated_request_chars"] > ceiling:
            regressions.append(
                f"request chars {base['max_estimated_request_chars']} → "
                f"{result['max_estimated_request_chars']} (>5%)"
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
    """Forced compaction must not cost quality.

    This holds for live routes too, where no committed baseline applies: the
    control run of the same scenario and model is the reference.
    """
    by_case = {
        (result["scenario"], result["model"], result.get("trial", 1), result["variant"]): result
        for result in results
    }
    comparisons = []
    for (scenario, model, trial, variant), result in by_case.items():
        if variant != "compacted":
            continue
        control = by_case.get((scenario, model, trial, "control"))
        if control is None:
            continue
        regressions = []
        if result["score"] < control["score"]:
            regressions.append(f"compacted score {control['score']} → {result['score']}")
        comparisons.append(
            {
                "key": f"{scenario}/{model}/trial-{trial}/compacted-vs-control",
                "status": "regression" if regressions else "ok",
                "regressions": regressions,
                "passed": not regressions,
            }
        )
    return comparisons


def write_baseline(results: list[dict[str, Any]]) -> None:
    payload = {
        "schema": "uagent.eval.baseline.v1",
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
                "max_estimated_request_chars": result["max_estimated_request_chars"],
                "cumulative_estimated_request_chars": result["cumulative_estimated_request_chars"],
                "tool_result_chars": result["tool_result_chars"],
                "initial_schema_chars": result["initial_schema_chars"],
            }
            for result in sorted(results, key=baseline_key)
        },
    }
    BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
    BASELINE_PATH.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"baseline written: {BASELINE_PATH.relative_to(ROOT)} ({len(results)} cases)")


# --- repeated trials and live cost authority --------------------------------


def wilson_interval(successes: int, trials: int, z: float = 1.96) -> tuple[float, float]:
    if trials <= 0:
        return (0.0, 0.0)
    probability = successes / trials
    denominator = 1 + z * z / trials
    center = (probability + z * z / (2 * trials)) / denominator
    radius = (
        z
        * math.sqrt(probability * (1 - probability) / trials + z * z / (4 * trials * trials))
        / denominator
    )
    return (max(0.0, center - radius), min(1.0, center + radius))


def pass_probabilities(successes: int, trials: int, requested_k: int) -> dict[str, Any]:
    if trials <= 0:
        return {
            "trials": 0,
            "successes": 0,
            "k": 0,
            "pass@1": 0.0,
            "pass@k": 0.0,
            "pass^k": 0.0,
            "pass@1_ci95": [0.0, 0.0],
        }
    k = min(max(1, requested_k), trials)
    denominator = math.comb(trials, k)
    pass_at_k = 1.0
    if trials - successes >= k:
        pass_at_k -= math.comb(trials - successes, k) / denominator
    pass_power_k = math.comb(successes, k) / denominator if successes >= k else 0.0
    low, high = wilson_interval(successes, trials)
    return {
        "trials": trials,
        "successes": successes,
        "k": k,
        "pass@1": round(successes / trials, 6),
        "pass@k": round(pass_at_k, 6),
        "pass^k": round(pass_power_k, 6),
        "pass@1_ci95": [round(low, 6), round(high, 6)],
    }


def trial_summaries(results: list[dict[str, Any]], requested_k: int) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str, str, str, str], list[dict[str, Any]]] = collections.defaultdict(
        list
    )
    for result in results:
        key = (
            str(result.get("scenario") or "?"),
            str(result.get("variant") or "control"),
            str(result.get("model") or "?"),
            str(result.get("route") or result.get("model") or "?"),
            str(result.get("cohort") or "legacy"),
        )
        grouped[key].append(result)
    summaries = []
    for key, trials in sorted(grouped.items()):
        scenario, variant, model, route, cohort = key
        successful = sum(bool(result.get("passed")) for result in trials)
        statistics = pass_probabilities(successful, len(trials), requested_k)
        statistics.update(
            {
                "scenario": scenario,
                "variant": variant,
                "model": model,
                "route": route,
                "cohort": cohort,
                "mean_model_requests": round(
                    sum(int(result.get("model_requests") or 0) for result in trials) / len(trials),
                    3,
                ),
                "mean_cumulative_context_chars": round(
                    sum(
                        int(result.get("cumulative_estimated_request_chars") or 0)
                        for result in trials
                    )
                    / len(trials)
                ),
                "mean_tool_result_chars": round(
                    sum(int(result.get("tool_result_chars") or 0) for result in trials)
                    / len(trials)
                ),
            }
        )
        summaries.append(statistics)
    return summaries


class LiveCostBlocker(RuntimeError):
    """The runner cannot prove that a live route stays inside its authority."""


def load_cost_authority(path: Path | None, models: list[str]) -> dict[str, Any]:
    try:
        return load_authority(path, models)
    except AuthorityError as error:
        raise LiveCostBlocker(f"live evaluation blocked: {error}") from error


def validate_live_plan(authority: dict[str, Any], jobs: list[tuple[Any, ...]], trials: int) -> None:
    sessions = collections.Counter(model for _, _, model in jobs)
    for model, count in sessions.items():
        declaration = authority["routes"][model]
        if declaration["mode"] != "non-billable-cheap":
            continue
        planned = count * trials
        maximum = declaration["limits"]["max_sessions"]
        if planned > maximum:
            raise LiveCostBlocker(
                f"live evaluation blocked: {model} plans {planned} sessions, above its cheap "
                f"authority limit of {maximum}"
            )


def apply_live_authority(env: dict[str, str], declaration: dict[str, Any]) -> None:
    if declaration["mode"] != "non-billable-cheap":
        return
    limits = declaration["limits"]
    env.update(
        {
            "UAGENT_MAX_STEPS": str(limits["max_model_calls"]),
            "UAGENT_MAX_TOOL_CALLS": str(limits["max_tool_calls"]),
            "UAGENT_MAX_TOKENS": str(limits["max_output_tokens_per_call"]),
            "UAGENT_MAX_TURN_SECONDS": str(limits["max_session_seconds"]),
            "UAGENT_REQUEST_TIMEOUT": str(limits["max_session_seconds"]),
            "UAGENT_FIRST_EVENT_TIMEOUT": str(limits["max_session_seconds"]),
            "UAGENT_STREAM_IDLE_TIMEOUT": str(limits["max_session_seconds"]),
            "UAGENT_MAX_TURN_COST": "0",
            "UAGENT_SESSION_BUDGET": "0",
            "UAGENT_OPENROUTER_FALLBACKS": "0",
        }
    )


def account_live_result(
    result: dict[str, Any], declaration: dict[str, Any], spent: float, cap: float
) -> float:
    if declaration["mode"] == "reported-cost":
        return account_live_cost(result, spent, cap)
    limits = declaration["limits"]
    usage = result.get("usage")
    if not isinstance(usage, dict):
        usage = {}
    checks = {
        "model calls": (int(result.get("model_requests") or 0), limits["max_model_calls"]),
        "tool calls": (int(result.get("tool_calls") or 0), limits["max_tool_calls"]),
        "output tokens": (
            int(usage.get("output") or 0),
            limits["max_model_calls"] * limits["max_output_tokens_per_call"],
        ),
        "session milliseconds": (
            int(float(result.get("elapsed_seconds") or 0) * 1000),
            limits["max_session_seconds"] * 1000,
        ),
    }
    for name, (used, maximum) in checks.items():
        if used > maximum:
            raise LiveCostBlocker(
                f"live evaluation blocked after {result.get('model')}: {name} {used} exceeded "
                f"the enforced cheap-route limit {maximum}"
            )
    return spent


def account_live_cost(result: dict[str, Any], spent: float, cap: float) -> float:
    usage = result.get("usage") or {}
    if not isinstance(usage, dict) or not usage.get("cost_reported"):
        raise LiveCostBlocker(
            f"live evaluation blocked after {result.get('model')}: provider cost was unavailable"
        )
    cost = float(usage.get("cost") or 0)
    if cost < 0:
        raise LiveCostBlocker("live evaluation blocked: provider reported a negative cost")
    updated = spent + cost
    if updated > cap + 1e-9:
        raise LiveCostBlocker(
            f"live evaluation stopped: reported aggregate cost ${updated:.6f} exceeded "
            f"the ${cap:.6f} ceiling"
        )
    return updated


def deterministic_jobs(jobs: list[tuple[Any, ...]], seed: int, trial: int) -> list[tuple[Any, ...]]:
    ordered = list(jobs)
    random.Random(seed + trial).shuffle(ordered)
    return ordered


def eval_self_test() -> int:
    fixture = json.loads(SELF_TEST_PATH.read_text(encoding="utf-8"))
    summary = trial_summaries(fixture["results"], fixture["k"])
    failures = []
    if len(summary) != 1:
        failures.append(f"produced {len(summary)} trial groups, want 1")
    else:
        for field, expected in fixture["expected"].items():
            if summary[0].get(field) != expected:
                failures.append(f"{field}={summary[0].get(field)}, want {expected}")
    for planted in fixture["blocked_cost_cases"]:
        try:
            account_live_cost(planted["result"], planted["spent"], planted["cap"])
        except LiveCostBlocker:
            continue
        failures.append(f"cost guard accepted planted case {planted['name']}")
    cheap_document = json.loads(CHEAP_AUTHORITY_SELF_TEST_PATH.read_text(encoding="utf-8"))
    cheap = normalize_route_authority("provider/model", cheap_document["routes"]["provider/model"])
    if cheap["mode"] != "non-billable-cheap":
        failures.append("valid cheap authority did not normalize")
    reported = normalize_route_authority(
        "fixture/reported", {"reports_cost": True, "enforces_hard_budget": True}
    )
    if reported["mode"] != "reported-cost":
        failures.append("valid reported-cost authority did not normalize")
    blocked_authorities = [
        {
            "name": "cheap-without-explicit-cheap-flag",
            "value": {"non_billable": True, "limits": cheap["limits"]},
        },
        {
            "name": "string-false-authority-flags",
            "value": {
                "non_billable": "false",
                "cheap": "false",
                "limits": cheap["limits"],
            },
        },
        {
            "name": "cheap-limit-above-global-ceiling",
            "value": {
                "non_billable": True,
                "cheap": True,
                "limits": {**cheap["limits"], "max_model_calls": 9},
            },
        },
    ]
    for planted in blocked_authorities:
        try:
            normalize_route_authority("provider/model", planted["value"])
        except AuthorityError:
            continue
        failures.append(f"authority guard accepted planted case {planted['name']}")
    env = {}
    apply_live_authority(env, cheap)
    expected_env = {
        "UAGENT_MAX_STEPS": "3",
        "UAGENT_MAX_TOOL_CALLS": "4",
        "UAGENT_MAX_TOKENS": "512",
        "UAGENT_MAX_TURN_SECONDS": "30",
        "UAGENT_OPENROUTER_FALLBACKS": "0",
    }
    for name, expected in expected_env.items():
        if env.get(name) != expected:
            failures.append(f"cheap authority env {name}={env.get(name)}, want {expected}")
    try:
        validate_live_plan(
            {"routes": {"provider/model": cheap}},
            [(None, None, "provider/model")],
            3,
        )
    except LiveCostBlocker:
        pass
    else:
        failures.append("cheap session-count guard accepted 3 sessions above limit 2")
    try:
        account_live_result(
            {
                "model": "provider/model",
                "model_requests": 4,
                "tool_calls": 0,
                "elapsed_seconds": 1,
                "usage": {"output": 1, "cost_reported": False},
            },
            cheap,
            0.0,
            0.0,
        )
    except LiveCostBlocker:
        pass
    else:
        failures.append("cheap post-run guard accepted excess model calls")
    jobs = [("a",), ("b",), ("c",)]
    if deterministic_jobs(jobs, 17, 2) != deterministic_jobs(jobs, 17, 2):
        failures.append("trial ordering was not deterministic")
    print(f"planted alternating trials  {summary}")
    print(f"planted cost blocks         {len(fixture['blocked_cost_cases'])}")
    print(f"planted authority blocks    {len(blocked_authorities) + 2}")
    for failure in failures:
        print(f"SELF-TEST FAILED: {failure}")
    return 1 if failures else 0


# --- reporting ---------------------------------------------------------------


def print_results(results, comparisons, summaries):
    print(
        f"{'scenario/variant':<38} {'score':>7} {'req':>4} {'idle':>5} {'tools':>6} "
        f"{'batch':>5} {'maxctx':>7} {'cumctx':>8} {'results':>8} {'wall':>7}"
    )
    repeated = any(int(result.get("trial") or 1) > 1 for result in results)
    for result in results:
        label = baseline_key(result)
        if repeated:
            label += f"#t{result['trial']}"
        print(
            f"{label:<38} "
            f"{result['score']}/{result['checks_total']:<5} "
            f"{result['model_requests']:>4} {result['no_action_rounds']:>5} "
            f"{result['tool_calls']:>6} "
            f"{result['max_batch']:>5} {result['max_estimated_request_chars']:>7} "
            f"{result['cumulative_estimated_request_chars']:>8} "
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
    for summary in summaries:
        print(
            "TRIALS: "
            f"{summary['scenario']}/{summary['variant']} {summary['model']} "
            f"cohort={summary['cohort']} n={summary['trials']} "
            f"pass@1={summary['pass@1']:.3f} "
            f"pass@{summary['k']}={summary['pass@k']:.3f} "
            f"pass^{summary['k']}={summary['pass^k']:.3f} "
            f"CI95={summary['pass@1_ci95']} rounds={summary['mean_model_requests']:.2f}"
        )


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, help="uagent binary to evaluate")
    parser.add_argument("--scenario", action="append", default=[], help="name; repeatable")
    parser.add_argument("--variant", action="append", default=[], help="name; repeatable")
    parser.add_argument("--check", action="store_true", help="fail on baseline regressions")
    parser.add_argument("--update", action="store_true", help="rewrite the committed baseline")
    parser.add_argument("--report", type=Path, help="write the full JSON report")
    parser.add_argument("--prompt-overlay", type=Path, help="UAGENT_PROMPT_OVERLAY for every run")
    parser.add_argument("--run", action="store_true", help="allow opt-in live provider calls")
    parser.add_argument("--model", action="append", default=[], help="model route; repeatable")
    parser.add_argument(
        "--cost-authority",
        type=Path,
        help="route authority for reported-cost or explicitly non-billable cheap live runs",
    )
    parser.add_argument("--max-cost", type=float, default=0.10, help="aggregate reported USD cap")
    parser.add_argument("--trials", type=int, default=1, help="fresh isolated runs per case")
    parser.add_argument("--pass-k", type=int, default=0, help="k for pass@k and pass^k")
    parser.add_argument("--seed", type=int, default=0, help="deterministic trial ordering seed")
    parser.add_argument("--timeout", type=int, default=120, help="seconds per run")
    parser.add_argument("--toolset", choices=("full", "lean"), default="full")
    parser.add_argument(
        "--self-test", action="store_true", help="validate trial statistics and cost guards"
    )
    arguments = parser.parse_args()
    if arguments.self_test:
        return arguments
    if arguments.binary is None:
        parser.error("binary is required unless --self-test is used")
    arguments.binary = arguments.binary.resolve()
    if not arguments.binary.is_file():
        parser.error(f"binary does not exist: {arguments.binary}")
    if arguments.max_cost < 0:
        parser.error("--max-cost cannot be negative")
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    if arguments.trials <= 0:
        parser.error("--trials must be positive")
    if arguments.pass_k < 0:
        parser.error("--pass-k cannot be negative")
    if arguments.pass_k == 0:
        arguments.pass_k = min(3, arguments.trials)
    if arguments.trials > 1 and not arguments.scenario:
        parser.error("repeated trials require at least one explicit --scenario")
    if arguments.run and not arguments.model:
        parser.error("--run requires at least one --model")
    if arguments.model and not arguments.run:
        parser.error("--model makes provider calls and therefore requires --run")
    if arguments.update and arguments.run:
        parser.error("baselines are hermetic; --update cannot use live runs")
    if arguments.update and arguments.trials != 1:
        parser.error("baseline updates require --trials 1")
    if arguments.cost_authority and not arguments.run:
        parser.error("--cost-authority is only used with --run")
    if arguments.run:
        try:
            arguments.cost_authority_data = load_cost_authority(
                arguments.cost_authority, arguments.model
            )
            if (
                any(
                    declaration["mode"] == "reported-cost"
                    for declaration in arguments.cost_authority_data["routes"].values()
                )
                and arguments.max_cost <= 0
            ):
                parser.error("reported-cost live routes require a positive --max-cost")
        except LiveCostBlocker as error:
            parser.error(str(error))
    return arguments


def graduation_notes(results: list[dict[str, Any]]) -> list[str]:
    """A capability scenario that holds green belongs in the regression tier.

    A live-only scenario never does: hermetically its script hands it the
    answer, so gating it would assert nothing. Suggesting it every run would
    only teach the reader to skip these lines.
    """
    return [
        f"{baseline_key(result)}: capability scenario is green — set "
        f'"tier": "regression" to gate it'
        for result in results
        if result["tier"] == "capability" and result["passed"] and not result["live_only"]
    ]


def main() -> int:
    arguments = parse_args()
    if arguments.self_test:
        return eval_self_test()
    scenarios = load_scenarios(arguments.scenario)
    models = arguments.model if arguments.run else ["eval"]
    jobs = []
    for scenario in scenarios:
        variants = scenario.get("variants", ["control"])
        if arguments.variant:
            variants = [name for name in variants if name in arguments.variant]
        for model in models:
            for variant in variants:
                jobs.append((scenario, variant, model))
    if not jobs:
        raise SystemExit("no scenario variants selected")
    if arguments.run:
        try:
            validate_live_plan(arguments.cost_authority_data, jobs, arguments.trials)
        except LiveCostBlocker as error:
            raise SystemExit(str(error)) from error

    results = []
    spent = 0.0
    blocker = None
    for trial in range(1, arguments.trials + 1):
        for scenario, variant, model in deterministic_jobs(jobs, arguments.seed, trial):
            declaration = arguments.cost_authority_data["routes"][model] if arguments.run else None
            reported = declaration is not None and declaration["mode"] == "reported-cost"
            remaining = arguments.max_cost - spent if reported else None
            if reported and remaining <= 1e-9:
                blocker = LiveCostBlocker(
                    f"live evaluation stopped at the ${arguments.max_cost:.6f} aggregate ceiling"
                )
                break
            result = run_case(
                arguments.binary,
                scenario,
                variant,
                model,
                arguments,
                trial=trial,
                trial_seed=arguments.seed + trial,
                budget=remaining,
            )
            results.append(result)
            if arguments.run:
                try:
                    spent = account_live_result(result, declaration, spent, arguments.max_cost)
                except LiveCostBlocker as error:
                    blocker = error
                    break
        if blocker:
            break

    comparisons = variant_comparisons(results)
    if not arguments.run:
        comparisons += compare(results, load_baseline())
    summaries = trial_summaries(results, arguments.pass_k)
    print_results(results, comparisons, summaries)
    if blocker:
        print(f"BLOCKED: {blocker}", file=sys.stderr)
    if arguments.update:
        write_baseline(results)
    selected_authorities = arguments.cost_authority_data["routes"] if arguments.run else {}
    reported_selected = any(
        declaration["mode"] == "reported-cost" for declaration in selected_authorities.values()
    )
    cheap_routes = {
        model: declaration["limits"]
        for model, declaration in selected_authorities.items()
        if declaration["mode"] == "non-billable-cheap"
    }
    cheap_sessions = collections.Counter(
        result["model"]
        for result in results
        if result.get("authority_mode") == "non-billable-cheap"
    )
    report = {
        "schema": "uagent.eval.v2",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "live": arguments.run,
        "binary": {
            "path": str(arguments.binary),
            "bytes": arguments.binary.stat().st_size,
            "sha256": hashlib.sha256(arguments.binary.read_bytes()).hexdigest(),
        },
        "toolset": arguments.toolset,
        "prompt_overlay": str(arguments.prompt_overlay) if arguments.prompt_overlay else None,
        "trial_policy": {
            "trials": arguments.trials,
            "pass_k": arguments.pass_k,
            "seed": arguments.seed,
        },
        "live_authority": {
            "sha256": (arguments.cost_authority_data.get("sha256") if arguments.run else None),
            "routes": {
                model: declaration["mode"] for model, declaration in selected_authorities.items()
            },
        },
        "aggregate_cost": {
            "ceiling": arguments.max_cost if arguments.run and reported_selected else None,
            "reported": round(spent, 9) if arguments.run and reported_selected else None,
            "authoritative": bool(arguments.run and reported_selected and not blocker),
        },
        "non_billable_cheap": {
            "routes": cheap_routes,
            "sessions": dict(sorted(cheap_sessions.items())),
            "authoritative": bool(arguments.run and cheap_routes and not blocker),
        },
        "blocker": str(blocker) if blocker else None,
        "results": results,
        "trial_summaries": summaries,
        "comparisons": comparisons,
    }
    if arguments.report:
        arguments.report.parent.mkdir(parents=True, exist_ok=True)
        arguments.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if blocker:
        return 2
    if not all(result["passed"] for result in results if result["tier"] == "regression"):
        return 1
    if arguments.check and not all(item["passed"] for item in comparisons):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
