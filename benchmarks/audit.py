#!/usr/bin/env python3
"""Five-axis improvement dashboard for one µAgent build.

An iteration starts here. Each axis is measured rather than argued, and the
Pareto rule is that a change may improve one axis only if it does not regress
another:

    hardware       binary size, peak RSS
    token          schema and prompt bytes charged to every request
    speed          rebuild fanout, suite wall time
    capability     advertised tools, scenario and check coverage
    readability    lint probes, file sizes, slop counts

The sixth section is the anti-overfitting check: it compares the tool mix the
eval suite exercises against the tool mix real sessions actually used, because
a suite that drifts away from real usage stops measuring anything (Goodhart).

    python3 benchmarks/audit.py build/debug/uagent
    python3 benchmarks/audit.py build/debug/uagent --check
    python3 benchmarks/audit.py build/debug/uagent --update
"""

from __future__ import annotations

import argparse
import collections
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
BASELINE_PATH = ROOT / "benchmarks" / "baselines" / "audit.json"
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "benchmarks"))

# isort: off
from integration_support import Server, base_env, event  # noqa: E402
from eval import (  # noqa: E402
    load_scenarios,
    measured_command,
    peak_rss,
    read_trace,
    trace_metrics,
)
from slopscan import CHECKS as SLOP_CHECKS  # noqa: E402
from slopscan import scan as slop_scan  # noqa: E402

# isort: on

# A tool below this share of real calls is not worth failing a build over; one
# above it with no scenario means the suite is measuring the wrong thing.
COVERAGE_FLOOR = 0.05


def profile_copies(home: Path) -> bool:
    """Copy the machine's memories and skills into a throwaway HOME.

    The hermetic probe runs with an empty HOME and no memory, which is the
    floor: a session on a machine someone uses carries stored memories and
    installed skills, and both are charged on every request. Copying rather
    than pointing at the real directories keeps the probe unable to write to
    them, and leaves this measurement absent rather than wrong on a fresh
    checkout.
    """
    source = Path.home() / ".uagent"
    copied = False
    for name in ("memory", "skills"):
        origin = source / name
        if not origin.is_dir():
            continue
        shutil.copytree(origin, home / ".uagent" / name, dirs_exist_ok=True)
        copied = True
    return copied


def probe_session(binary: Path, with_profile: bool = False) -> dict[str, Any]:
    """One hermetic turn, to read the surface the model is actually charged for."""
    with tempfile.TemporaryDirectory(prefix="uagent-audit-") as temp:
        root = Path(temp)
        home = root / "home"
        home.mkdir()
        if with_profile and not profile_copies(home):
            return {}
        trace = root / "trace.jsonl"
        flags = ["--json", f"--debug={trace}", "-p", "hi"]
        if not with_profile:
            flags.insert(1, "--no-memory")
        with Server([lambda *_: event({"content": "ok"})]) as server:
            env = base_env(home, server.url)
            process = subprocess.run(
                measured_command(binary, flags),
                cwd=root,
                env=env,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
            )
        records = read_trace(trace)
    metrics = trace_metrics(records)
    request = next((r.get("data", {}) for r in records if r.get("event") == "model_request"), {})
    schemas = request.get("schema_snapshot") or []
    per_tool = {}
    for entry in schemas:
        function = entry.get("function", entry)
        per_tool[function.get("name", "?")] = len(json.dumps(entry, separators=(",", ":")))
    messages = request.get("messages") or []
    return {
        "peak_rss_bytes": peak_rss(process.stderr),
        "system_chars": len(str(messages[0].get("content", ""))) if messages else 0,
        "schema_chars": int(request.get("schema_chars") or 0),
        "advertised_tools": len(schemas),
        "per_tool_bytes": dict(sorted(per_tool.items(), key=lambda item: -item[1])),
        "cumulative_request_chars": metrics["cumulative_estimated_request_chars"],
        "tool_result_chars": metrics["tool_result_chars"],
        "model_duration_ms": metrics["model_duration_ms"],
    }


def rebuild_fanout() -> dict[str, int]:
    """How many translation units a header edit rebuilds, from the real depfiles.

    This is the build-speed cost of a header's coupling, measured rather than
    guessed: editing a header near the top of this list is what makes an
    iteration slow.
    """
    counts: collections.Counter[str] = collections.Counter()
    directories = sorted(
        {path.parent for path in (ROOT / "build").glob("*/CMakeFiles/uagent_core.dir")}
    )
    if not directories:
        return {}
    # One build directory, and each header counted once per translation unit:
    # the number has to mean "objects rebuilt", not "lines in depfiles".
    for depfile in directories[0].rglob("*.o.d"):
        text = depfile.read_text(errors="replace").replace("\\\n", " ")
        _, _, dependencies = text.partition(":")
        seen: set[str] = set()
        for token in dependencies.split():
            if not token.endswith(".h"):
                continue
            path = Path(token)
            if not path.is_absolute():
                path = (depfile.parent / path).resolve()
            try:  # third-party and system headers are not ours to decouple
                relative = path.relative_to(ROOT)
            except ValueError:
                continue
            if relative.parts[0] in ("include", "src"):
                seen.add(str(relative))
        counts.update(seen)
    return dict(counts.most_common(6))


def tool_surface() -> list[dict[str, str]]:
    """The generated reference is the committed description of the tool surface."""
    path = ROOT / "skills" / "uagent-config" / "references" / "tools.md"
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        # A description may itself contain an escaped pipe, so the row is only
        # required to have at least the columns before it.
        cells = [cell.strip() for cell in line.split("|")[1:-1]]
        if len(cells) >= 8 and cells[0].startswith("`") and cells[1].isdigit():
            rows.append({"name": cells[0].strip("`"), "bytes": cells[1], "when": cells[5]})
    return rows


def scenario_coverage() -> dict[str, Any]:
    scenarios = load_scenarios([])
    tools: collections.Counter[str] = collections.Counter()
    checks = 0
    tiers: collections.Counter[str] = collections.Counter()
    for scenario in scenarios:
        tiers[scenario.get("tier", "regression")] += 1
        checks += len(scenario.get("checks", {}))
        for rule in scenario.get("script", []):
            for call in rule.get("respond", {}).get("tool_calls", []):
                tools[call["name"]] += 1
    return {
        "scenarios": len(scenarios),
        "checks": checks,
        "tiers": dict(tiers),
        "tool_calls": dict(tools),
    }


def evaluation_resources() -> dict[str, Any]:
    """Committed scenario telemetry, descriptive rather than a global gate."""
    baseline = ROOT / "benchmarks" / "baselines" / "hermetic.json"
    if not baseline.is_file():
        return {}
    scenarios = json.loads(baseline.read_text(encoding="utf-8")).get("scenarios", {})
    values = list(scenarios.values())
    return {
        "cases": len(values),
        "cumulative_request_chars": sum(
            int(value.get("cumulative_estimated_request_chars") or 0) for value in values
        ),
        "tool_result_chars": sum(int(value.get("tool_result_chars") or 0) for value in values),
        "max_request_chars": max(
            (int(value.get("max_estimated_request_chars") or 0) for value in values),
            default=0,
        ),
    }


def real_tool_mix(history: Path, days: int) -> dict[str, int]:
    """What tools real sessions actually called, from the session journals."""
    cutoff = time.time() - days * 86400
    counts: collections.Counter[str] = collections.Counter()
    if not history.is_dir():
        return {}
    for path in history.rglob("*.events.jsonl"):
        if path.stat().st_mtime < cutoff:
            continue
        for line in path.read_text(errors="replace").splitlines():
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("type") == "tool.call":
                counts[record.get("data", {}).get("name", "?")] += 1
    return dict(counts.most_common())


def lint_probes() -> dict[str, int]:
    """Slop signals, in the whole tree rather than in the last diff.

    Centralising is how slop appears: the old body stays behind, a caller keeps
    its copy, a doc keeps the old name. None of that shows up in the diff that
    introduces the next change.
    """
    slop = collections.Counter(item["kind"] for item in slop_scan(sorted(SLOP_CHECKS)))
    probe = subprocess.run(
        [
            "uv",
            "run",
            "--frozen",
            "ruff",
            "check",
            "--select",
            "ARG,ERA,F401,F841",
            "--output-format",
            "concise",
            "tests",
            "benchmarks",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    findings = [line for line in probe.stdout.splitlines() if ": " in line]
    markers = subprocess.run(
        ["git", "grep", "-cE", r"TODO|FIXME|HACK", "--", "src", "include"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    sizes = sorted(
        (
            (len(path.read_text(errors="replace").splitlines()), str(path.relative_to(ROOT)))
            for path in list((ROOT / "src").rglob("*.cc")) + list((ROOT / "include").rglob("*.h"))
        ),
        reverse=True,
    )
    return {
        "lint_findings": len(findings),
        "slop_findings": sum(slop.values()),
        "slop": {kind: slop[kind] for kind in sorted(SLOP_CHECKS)},
        "marker_files": len([line for line in markers.stdout.splitlines() if line]),
        "largest_file_lines": sizes[0][0] if sizes else 0,
        "largest_file": sizes[0][1] if sizes else "",
    }


def collect(binary: Path, arguments) -> dict[str, Any]:
    session = probe_session(binary)
    profile = probe_session(binary, with_profile=True)
    surface = tool_surface()
    always = sum(int(row["bytes"]) for row in surface if row["when"] == "always")
    coverage = scenario_coverage()
    resources = evaluation_resources()
    real = real_tool_mix(Path(arguments.history).expanduser(), arguments.since)
    # History outlives releases: a journal can name a tool this build no longer
    # has, and that is a rename to acknowledge, not a coverage gap to chase.
    current = {row["name"] for row in surface}
    retired = sorted(name for name in real if name not in current)
    live = {name: count for name, count in real.items() if name in current}
    total_real = sum(live.values()) or 1
    uncovered = [
        name
        for name, count in live.items()
        if count / total_real >= COVERAGE_FLOOR and name not in coverage["tool_calls"]
    ]
    return {
        "schema": "uagent.audit.v1",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "hardware": {
            "binary_bytes": binary.stat().st_size,
            "peak_rss_bytes": session["peak_rss_bytes"],
        },
        "token": {
            "system_chars": session["system_chars"],
            "schema_chars": session["schema_chars"],
            "advertised_tools": session["advertised_tools"],
            "always_on_schema_bytes": always,
            "largest_schemas": dict(list(session["per_tool_bytes"].items())[:5]),
        },
        # Reported, never gated: it depends on what this machine has stored,
        # so a baseline built from it would fail on someone else's memories.
        "profile_token": {
            "system_chars": profile.get("system_chars", 0),
            "schema_chars": profile.get("schema_chars", 0),
            "advertised_tools": profile.get("advertised_tools", 0),
            "only_with_profile": sorted(
                set(profile.get("per_tool_bytes", {})) - set(session["per_tool_bytes"])
            ),
        },
        "speed": {"rebuild_fanout": rebuild_fanout()},
        "capability": {
            "surface_tools": len(surface),
            **coverage,
        },
        "trajectory": {
            "probe": {
                "cumulative_request_chars": session.get("cumulative_request_chars", 0),
                "tool_result_chars": session.get("tool_result_chars", 0),
                "model_duration_ms": session.get("model_duration_ms", 0),
            },
            "hermetic_baseline": resources,
        },
        "readability": lint_probes(),
        "representativeness": {
            "real_calls": total_real if live else 0,
            "real_mix": dict(list(live.items())[:8]),
            "uncovered_above_floor": uncovered,
            "retired_names_in_history": retired,
        },
    }


def compare(current: dict[str, Any], baseline: dict[str, Any]) -> list[str]:
    """Only the axes that are machine-independent are allowed to fail a build."""
    regressions = []
    for axis, field, tolerance in (
        ("token", "system_chars", 1.05),
        ("token", "schema_chars", 1.05),
        ("token", "always_on_schema_bytes", 1.05),
        ("readability", "lint_findings", 1.0),
        ("readability", "slop_findings", 1.0),
    ):
        before = baseline.get(axis, {}).get(field)
        after = current.get(axis, {}).get(field)
        if before is None or after is None:
            continue
        ceiling = before * tolerance + (16 if tolerance > 1 else 0)
        if after > ceiling:
            regressions.append(f"{axis}.{field} {before} → {after}")
    covered_before = set(baseline.get("capability", {}).get("tool_calls", {}))
    covered_after = set(current.get("capability", {}).get("tool_calls", {}))
    if covered_before - covered_after:
        regressions.append(f"scenario coverage lost: {sorted(covered_before - covered_after)}")
    uncovered = current.get("representativeness", {}).get("uncovered_above_floor", [])
    if uncovered:
        regressions.append(f"no scenario exercises heavily used tools: {uncovered}")
    return regressions


def render(report: dict[str, Any]) -> None:
    token = report["token"]
    print(
        "hardware      binary {binary_bytes:,} B   peak RSS {peak_rss_bytes:,} B".format(
            **report["hardware"]
        )
    )
    print(
        f"token         system {token['system_chars']:,} + schemas {token['schema_chars']:,} chars "
        f"over {token['advertised_tools']} advertised tools "
        f"({token['always_on_schema_bytes']:,} B always-on)"
    )
    print(f"              largest: {token['largest_schemas']}")
    profile = report.get("profile_token", {})
    if profile.get("system_chars"):
        print(
            f"              this machine's profile: system {profile['system_chars']:,} + "
            f"schemas {profile['schema_chars']:,} chars over "
            f"{profile['advertised_tools']} tools"
            + (f", added: {profile['only_with_profile']}" if profile["only_with_profile"] else "")
        )
    fanout = report["speed"]["rebuild_fanout"]
    print(f"speed         rebuild fanout (TUs per header): {fanout}")
    capability = report["capability"]
    print(
        f"capability    {capability['surface_tools']} tools in the drift gate; "
        f"{capability['scenarios']} scenarios {capability['tiers']} with "
        f"{capability['checks']} checks over {sorted(capability['tool_calls'])}"
    )
    trajectory = report.get("trajectory", {})
    baseline_trajectory = trajectory.get("hermetic_baseline", {})
    probe_trajectory = trajectory.get("probe", {})
    if baseline_trajectory:
        print(
            "trajectory    {cases} cases · cumulative request "
            "{cumulative_request_chars:,} chars · tool results "
            "{tool_result_chars:,} chars · probe model "
            f"{probe_trajectory.get('model_duration_ms', 0):.0f}ms "
            "(descriptive, no global ceiling)".format(**baseline_trajectory)
        )
    readability = report["readability"]
    print(
        f"readability   {readability['lint_findings']} lint findings, "
        f"{readability['slop_findings']} slop findings {readability['slop']}, "
        f"{readability['marker_files']} files with TODO/FIXME, largest "
        f"{readability['largest_file']} at {readability['largest_file_lines']} lines"
    )
    representativeness = report["representativeness"]
    if representativeness["real_calls"]:
        share = {
            name: f"{100 * count / representativeness['real_calls']:.0f}%"
            for name, count in representativeness["real_mix"].items()
        }
        print(f"real usage    {representativeness['real_calls']} calls: {share}")
        uncovered = representativeness["uncovered_above_floor"]
        print(f"              uncovered above {COVERAGE_FLOOR:.0%}: {uncovered or 'none'}")
        if representativeness["retired_names_in_history"]:
            print(
                "              retired names in history: "
                f"{representativeness['retired_names_in_history']}"
            )
    else:
        print("real usage    no session journals found; coverage check skipped")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--check", action="store_true", help="fail on a regression")
    parser.add_argument("--update", action="store_true", help="rewrite the baseline")
    parser.add_argument("--json", type=Path, help="write the full report")
    parser.add_argument("--history", default="~/.uagent/history", help="session journals")
    parser.add_argument("--since", type=int, default=14, help="days of history to read")
    arguments = parser.parse_args()
    arguments.binary = arguments.binary.resolve()
    if not arguments.binary.is_file():
        parser.error(f"binary does not exist: {arguments.binary}")
    return arguments


def main() -> int:
    arguments = parse_args()
    report = collect(arguments.binary, arguments)
    render(report)
    if arguments.json:
        arguments.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    regressions = []
    if BASELINE_PATH.exists():
        regressions = compare(report, json.loads(BASELINE_PATH.read_text(encoding="utf-8")))
        for regression in regressions:
            print(f"REGRESSION: {regression}")
    if arguments.update:
        # After reporting, so rewriting the baseline still shows what moved.
        BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
        BASELINE_PATH.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"baseline written: {BASELINE_PATH.relative_to(ROOT)}")
        return 0
    if regressions and arguments.check:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
