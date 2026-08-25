#!/usr/bin/env python3
"""Scenario-driven behavioral evaluation for µAgent.

A scenario is one declarative JSON file under `benchmarks/scenarios/`: the
workspace fixture, the prompt, a scripted provider, and the checks that define a
good run. Scores and round counts are compared against committed baselines, so a
change that makes the agent spend more rounds, drop a batch, stop deduplicating
or lose its answer fails a gate instead of being argued about.

The hermetic suite scripts the provider, so it measures *harness* behavior — the
part this repository owns — not model quality. `--run --model` replays the same
scenarios against a real route as the periodic reality check; those runs are
billable and bounded by `--max-cost`.

    python3 benchmarks/eval.py build/debug/uagent --check
    python3 benchmarks/eval.py build/debug/uagent --update
    python3 benchmarks/eval.py build/release/uagent --run --model provider/model
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
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

# One HTTP/SSE fixture serves the integration suite and this harness; a second
# copy would drift from the transport the tests actually exercise.
sys.path.insert(0, str(ROOT / "tests"))

# isort: off
from integration_support import Server, event  # noqa: E402

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

    def __init__(self, rules: list[dict[str, Any]]) -> None:
        self.rules = rules
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
        for rule in self.rules:
            if self.matches(rule.get("when", {}), body, serialized, results, last):
                return self.payload(rule["respond"])
        self.misses.append(last[:160])
        return event({"content": "SCRIPT-MISS"})

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
    """Per-step batching comes from ordering: calls follow the request that asked for them."""
    events: collections.Counter[str] = collections.Counter()
    batches: list[int] = []
    calls: list[dict[str, Any]] = []
    request_chars: list[int] = []
    current_messages = 0
    first: dict[str, Any] = {}
    compactions = 0
    for record in records:
        name = str(record.get("event", ""))
        data = record.get("data", {})
        events[name] += 1
        if name == "model_request":
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
        elif name == "tool_call":
            arguments = data.get("arguments", {})
            if isinstance(arguments, str):
                try:
                    arguments = json.loads(arguments)
                except json.JSONDecodeError:
                    arguments = {}
            calls.append({"name": data.get("name"), "arguments": arguments})
            if batches:
                batches[-1] += 1
        elif name == "compact_end" and data.get("outcome") == "ok":
            compactions += 1
    return {
        "model_requests": len(batches),
        "tool_calls": len(calls),
        "calls": calls,
        "max_batch": max(batches, default=0),
        "compactions": compactions,
        "events": events,
        "estimated_request_chars": sum(request_chars),
        "max_estimated_request_chars": max(request_chars, default=0),
        "initial_schema_chars": int(first.get("schema_chars") or 0),
    }


def case_environment(scenario: dict[str, Any], variant: str, arguments, mock) -> dict[str, str]:
    env = dict(os.environ)
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


def run_case(binary: Path, scenario: dict[str, Any], variant: str, model: str, arguments):
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
            script = Script(scenario.get("script", []))
            mock = Server([script])
        else:
            copy_user_config(home)
        try:
            env = case_environment(scenario, variant, arguments, mock)
            env["HOME"] = str(home)
            cli = [
                "--json",
                "--no-memory",
                f"--debug={trace_path}",
                "--model",
                model,
                "--budget",
                str(arguments.max_cost),
                "-p",
                scenario["prompt"],
            ]
            started = time.monotonic()
            process = subprocess.run(
                measured_command(binary, cli),
                cwd=workspace,
                env=env,
                stdin=subprocess.DEVNULL,
                text=True,
                capture_output=True,
                timeout=arguments.timeout + 15,
                check=False,
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
        )
        result = {
            "scenario": scenario["name"],
            "variant": variant,
            "model": model,
            "elapsed_seconds": round(elapsed, 3),
            "peak_rss_bytes": peak_rss(process.stderr),
            "usage": envelope.get("usage", {}),
            "model_requests": metrics["model_requests"],
            "tool_calls": metrics["tool_calls"],
            "max_batch": metrics["max_batch"],
            "compactions": metrics["compactions"],
            "estimated_request_chars": metrics["estimated_request_chars"],
            "max_estimated_request_chars": metrics["max_estimated_request_chars"],
            "initial_schema_chars": metrics["initial_schema_chars"],
            "checks": checks,
            "score": sum(1 for value in checks.values() if value),
            "checks_total": len(checks),
            "passed": all(checks.values()),
            "answer": answer,
            "script_misses": script.misses if script else [],
            "error": envelope.get("error")
            or (process.stderr.strip()[-400:] if process.returncode else None),
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


def evaluate(scenario, variant, *, answer, metrics, unchanged, returncode, bodies):
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
        if not result["passed"]:
            failed = [name for name, ok in result["checks"].items() if not ok]
            regressions.append("failed checks: " + ", ".join(failed))
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
        (result["scenario"], result["model"], result["variant"]): result for result in results
    }
    comparisons = []
    for (scenario, model, variant), result in by_case.items():
        if variant != "compacted":
            continue
        control = by_case.get((scenario, model, "control"))
        if control is None:
            continue
        regressions = []
        if result["score"] < control["score"]:
            regressions.append(f"compacted score {control['score']} → {result['score']}")
        comparisons.append(
            {
                "key": f"{scenario}/{model}/compacted-vs-control",
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
                "compactions": result["compactions"],
                "max_estimated_request_chars": result["max_estimated_request_chars"],
                "initial_schema_chars": result["initial_schema_chars"],
            }
            for result in sorted(results, key=baseline_key)
        },
    }
    BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
    BASELINE_PATH.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"baseline written: {BASELINE_PATH.relative_to(ROOT)} ({len(results)} cases)")


# --- reporting ---------------------------------------------------------------


def print_results(results, comparisons):
    print(
        f"{'scenario/variant':<34} {'score':>7} {'req':>4} {'tools':>6} "
        f"{'batch':>5} {'chars':>7} {'wall':>7}"
    )
    for result in results:
        print(
            f"{baseline_key(result):<34} "
            f"{result['score']}/{result['checks_total']:<5} "
            f"{result['model_requests']:>4} {result['tool_calls']:>6} "
            f"{result['max_batch']:>5} {result['max_estimated_request_chars']:>7} "
            f"{result['elapsed_seconds']:>6.1f}s"
        )
        if result["error"]:
            print(f"    error: {result['error']}")
    for comparison in comparisons:
        if comparison["status"] == "ok":
            continue
        detail = "; ".join(comparison["regressions"]) or "no baseline recorded"
        print(f"{comparison['status'].upper()}: {comparison['key']}: {detail}")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="uagent binary to evaluate")
    parser.add_argument("--scenario", action="append", default=[], help="name; repeatable")
    parser.add_argument("--variant", action="append", default=[], help="name; repeatable")
    parser.add_argument("--check", action="store_true", help="fail on baseline regressions")
    parser.add_argument("--update", action="store_true", help="rewrite the committed baseline")
    parser.add_argument("--report", type=Path, help="write the full JSON report")
    parser.add_argument("--prompt-overlay", type=Path, help="UAGENT_PROMPT_OVERLAY for every run")
    parser.add_argument("--run", action="store_true", help="allow opt-in live provider calls")
    parser.add_argument("--model", action="append", default=[], help="model route; repeatable")
    parser.add_argument("--max-cost", type=float, default=0.10, help="reported USD cap per run")
    parser.add_argument("--timeout", type=int, default=120, help="seconds per run")
    parser.add_argument("--toolset", choices=("full", "lean"), default="full")
    arguments = parser.parse_args()
    arguments.binary = arguments.binary.resolve()
    if not arguments.binary.is_file():
        parser.error(f"binary does not exist: {arguments.binary}")
    if arguments.max_cost <= 0:
        parser.error("--max-cost must be positive")
    if arguments.run and not arguments.model:
        parser.error("--run requires at least one --model")
    if arguments.model and not arguments.run:
        parser.error("--model makes provider calls and therefore requires --run")
    if arguments.update and arguments.run:
        parser.error("baselines are hermetic; --update cannot use live runs")
    return arguments


def main() -> int:
    arguments = parse_args()
    scenarios = load_scenarios(arguments.scenario)
    models = arguments.model if arguments.run else ["eval"]
    results = []
    for scenario in scenarios:
        variants = scenario.get("variants", ["control"])
        if arguments.variant:
            variants = [name for name in variants if name in arguments.variant]
        for model in models:
            for variant in variants:
                results.append(run_case(arguments.binary, scenario, variant, model, arguments))

    comparisons = variant_comparisons(results)
    if not arguments.run:
        comparisons += compare(results, load_baseline())
    print_results(results, comparisons)
    if arguments.update:
        write_baseline(results)
    report = {
        "schema": "uagent.eval.v1",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "live": arguments.run,
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
    if not all(result["passed"] for result in results):
        return 1
    if arguments.check and not all(item["passed"] for item in comparisons):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
