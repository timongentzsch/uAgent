#!/usr/bin/env python3
"""Capped, billable end-to-end µAgent workflow evaluation.

Runs the real binary against OpenRouter. This is intentionally excluded from
CTest and refuses to run without --run. Credentials are loaded into the child
environment but never printed or written to the fixture workspace.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any

DEFAULT_MODEL = "deepseek/deepseek-v4-flash"
DEFAULT_CONFIG = Path.home() / ".uagent" / ".config"
MAX_REPORTED_COST = 0.10
ASTROPY_COMMIT = "d16bfe05a744909de4b27f5875fe0d4ed41ce607"
ASTROPY_ARCHIVE_SHA256 = "4ffc67512585ebd76f93abe9544e3563f826ccf70e1576492d3f21eb8d3d4979"
ASTROPY_ARCHIVE_URL = "https://codeload.github.com/astropy/astropy/tar.gz/" + ASTROPY_COMMIT
SWE_BENCH_INSTANCE = "astropy__astropy-12907"
SWE_BENCH_URL = "https://huggingface.co/datasets/SWE-bench/SWE-bench_Verified"


def load_config(path: Path) -> dict[str, str]:
    """Read KEY=VALUE config without evaluating shell code."""
    if not path.exists():
        return {}
    values: dict[str, str] = {}
    variable = re.compile(r"\$(?:\{([A-Za-z_][A-Za-z0-9_]*)\}|([A-Za-z_][A-Za-z0-9_]*))")
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
            continue
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        scope = {**values, **os.environ}
        values[key] = variable.sub(
            lambda match, scope=scope: scope.get(match.group(1) or match.group(2), ""),
            value,
        )
    return values


@dataclass
class Result:
    name: str
    passed: bool
    detail: str
    elapsed: float
    usage: dict[str, Any]
    output: str
    events: list[dict[str, Any]]
    peak_rss_bytes: int = 0


def snapshot_archive(root: Path) -> Path:
    archive = root.parent / f"astropy-{ASTROPY_COMMIT}.tar.gz"
    if not archive.exists():
        request = urllib.request.Request(
            ASTROPY_ARCHIVE_URL,
            headers={"User-Agent": "uagent-live-benchmark"},
        )
        pending = archive.with_suffix(".download")
        with urllib.request.urlopen(request, timeout=60) as response:
            pending.write_bytes(response.read())
        pending.replace(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != ASTROPY_ARCHIVE_SHA256:
        raise RuntimeError(f"Astropy fixture checksum mismatch: {digest}")
    return archive


def write_fixture(root: Path) -> None:
    archive = snapshot_archive(root)
    prefix = f"astropy-{ASTROPY_COMMIT}/"
    wanted = (
        "astropy/modeling/separable.py",
        "astropy/modeling/tests/test_separable.py",
        "astropy/modeling/core.py",
    )
    with tarfile.open(archive, "r:gz") as bundle:
        for relative in wanted:
            source = bundle.extractfile(prefix + relative)
            if source is None:
                raise RuntimeError(f"missing fixture member: {relative}")
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.read())

    issue = root / "dataset" / "SWE_BENCH_ISSUE.md"
    issue.parent.mkdir()
    issue.write_text(
        f"""# {SWE_BENCH_INSTANCE}

Source: {SWE_BENCH_URL}
Repository: astropy/astropy
Base commit: {ASTROPY_COMMIT}

`separability_matrix` is correct for
`Pix2Sky_TAN() & Linear1D(10) & Linear1D(5)`, but a nested equivalent,
`Pix2Sky_TAN() & (Linear1D(10) & Linear1D(5))`, incorrectly marks the two
linear-model inputs and outputs as mutually dependent.
""",
        encoding="utf-8",
    )
    (root / "slow_analysis.py").write_text(
        """import time
time.sleep(4)
from pathlib import Path
source = Path("astropy/modeling/separable.py").read_text()
print("STATIC-REPORT: pinned SWE-bench fixture loaded")
print("STATIC-REPORT: _cstack definitions =", source.count("def _cstack"))
""",
        encoding="utf-8",
    )
    (root / "slow_probe.py").write_text(
        """import pathlib, sys, time
label = sys.argv[1]
path = pathlib.Path("probe.log")
with path.open("a", encoding="utf-8") as stream:
    stream.write(f"{label},start,{time.time()}\\n")
time.sleep(4)
with path.open("a", encoding="utf-8") as stream:
    stream.write(f"{label},end,{time.time()}\\n")
""",
        encoding="utf-8",
    )


def real_long_context(workspace: Path, target_chars: int = 1_150_000) -> str:
    archive = snapshot_archive(workspace)
    prefix = f"astropy-{ASTROPY_COMMIT}/"
    chunks: list[str] = []
    size = 0
    with tarfile.open(archive, "r:gz") as bundle:
        members = sorted(
            (
                member
                for member in bundle.getmembers()
                if member.isfile()
                and member.name.startswith(prefix)
                and Path(member.name).suffix in {".py", ".rst", ".md"}
            ),
            key=lambda member: member.name,
        )
        for member in members:
            source = bundle.extractfile(member)
            if source is None:
                continue
            content = source.read().decode("utf-8", errors="replace")
            relative = member.name[len(prefix) :]
            chunk = f"\n\n===== {relative} =====\n{content}"
            chunks.append(chunk)
            size += len(chunk)
            if size >= target_chars:
                break
    if size < target_chars:
        raise RuntimeError(f"Astropy fixture is too small: {size} characters")
    # Interactive µAgent consumes one line per turn. Preserve source boundaries
    # as literal escapes so the complete corpus remains one user message.
    return "".join(chunks).replace("\n", "\\n")


def read_events(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    events: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            events.append(value)
    return events


def session_usage(events: list[dict[str, Any]]) -> dict[str, Any]:
    for event in reversed(events):
        if event.get("event") == "session_end":
            usage = event.get("data", {}).get("usage", {})
            return usage if isinstance(usage, dict) else {}
    return {}


def raw_response_usage(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        event.get("data", {}).get("usage", {})
        for event in events
        if event.get("event") == "model_response"
        and isinstance(event.get("data", {}).get("usage"), dict)
    ]


def tool_events(events: list[dict[str, Any]], name: str) -> list[dict[str, Any]]:
    return [
        event
        for event in events
        if event.get("event") == "tool_result" and event.get("data", {}).get("name") == name
    ]


def trace_contains(events: list[dict[str, Any]], text: str) -> bool:
    needle = text.lower()
    return any(needle in json.dumps(event.get("data", {})).lower() for event in events)


def peak_rss_bytes(stderr: str) -> int:
    match = re.search(r"(\d+)\s+maximum resident set size", stderr)
    if match:
        return int(match.group(1))
    match = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", stderr)
    return int(match.group(1)) * 1024 if match else 0


def run_agent(
    binary: Path,
    workspace: Path,
    home: Path,
    base_env: dict[str, str],
    name: str,
    prompt: str,
    *,
    interactive: bool = False,
    overrides: dict[str, str] | None = None,
    timeout: int = 300,
) -> tuple[subprocess.CompletedProcess[str], list[dict[str, Any]], float]:
    trace = home / f"{name}.jsonl"
    env = dict(base_env)
    env.update(overrides or {})
    args = [str(binary), "--yolo", f"--debug={trace}"]
    if not interactive:
        args += ["-p", prompt]
        input_text = None
    else:
        input_text = prompt
    measured_args = args
    if Path("/usr/bin/time").is_file():
        if sys.platform == "darwin":
            measured_args = ["/usr/bin/time", "-l", *args]
        elif sys.platform.startswith("linux"):
            measured_args = ["/usr/bin/time", "-v", *args]
    started = time.monotonic()
    result = subprocess.run(
        measured_args,
        cwd=workspace,
        env=env,
        input=input_text,
        text=True,
        capture_output=True,
        timeout=timeout,
    )
    return result, read_events(trace), time.monotonic() - started


def evaluate_analysis(binary: Path, workspace: Path, home: Path, env: dict[str, str]) -> Result:
    prompt = """Analyze the pinned SWE-bench Verified issue in this fixture.
Your first tool call must be run with exactly
{"command":"python3 slow_analysis.py","timeout":1}; do not change that timeout.
After it backgrounds, use grep/read_file to inspect dataset/SWE_BENCH_ISSUE.md,
astropy/modeling/separable.py, and its tests; then collect the background
result. Report the exact nested-model defect, affected function and file, why
the right-hand matrix loses information, missing regression cases, and a
minimal repair direction. Do not modify files and do not inspect a gold patch.
The final answer must be exactly five bullets and at most 180 words: defect,
location, cause, regression tests, repair."""
    run, events, elapsed = run_agent(
        binary,
        workspace,
        home,
        env,
        "analysis",
        prompt,
        overrides={"UAGENT_MAX_TOKENS": "4000"},
    )
    output = run.stdout.strip()
    lower = output.lower()
    required = ("separable.py", "_cstack", "nested", "right", "test")
    backgrounded = any(
        "[backgrounded]" in event.get("data", {}).get("result", "")
        for event in tool_events(events, "run")
    )
    waited = bool(tool_events(events, "wait_background")) or trace_contains(
        events, "[Background result:"
    )
    passed = (
        run.returncode == 0 and all(term in lower for term in required) and backgrounded and waited
    )
    tool_names = [
        event.get("data", {}).get("name") for event in events if event.get("event") == "tool_result"
    ]
    raw_usage = raw_response_usage(events)
    detail = (
        f"tools={sum(e.get('event') == 'tool_result' for e in events)}, "
        f"backgrounded={backgrounded}, collected={waited}, "
        f"tool_names={tool_names}, raw_usage_events={len(raw_usage)}"
    )
    return Result(
        "long-analysis",
        passed,
        detail,
        elapsed,
        session_usage(events),
        output,
        events,
        peak_rss_bytes(run.stderr),
    )


def evaluate_research(binary: Path, workspace: Path, home: Path, env: dict[str, str]) -> Result:
    prompt = """Research current OpenRouter prompt-caching behavior and routing
controls. In your first action issue exactly two independent web_search calls in
the same tool batch: one for official prompt-caching documentation and one for
official provider routing/session-affinity documentation. Then synthesize a
short answer with source URLs, clearly separating documented facts from your
inference. Preserve provider/model-specific scope for any pricing claim. Do not
delegate."""
    run, events, elapsed = run_agent(binary, workspace, home, env, "research", prompt)
    output = run.stdout.strip()
    searches = tool_events(events, "web_search")
    successful_searches = [
        event for event in searches if event.get("data", {}).get("status") == "ok"
    ]
    parallel_batches = [
        event
        for event in events
        if event.get("event") == "tool_batch" and event.get("data", {}).get("parallel") is True
    ]
    passed = (
        run.returncode == 0
        and len(successful_searches) == 2
        and len(searches) <= 3
        and bool(parallel_batches)
        and "http" in output.lower()
        and "cache" in output.lower()
    )
    lines = [line.lower() for line in output.splitlines()]
    pricing_scoped = all(
        any(scope in line for scope in ("anthropic", "alibaba", "provider-specific"))
        for line in lines
        if "1.25" in line
    )
    top_level_scoped = all(
        not (
            "top-level" in line
            and any(provider in line for provider in ("vertex", "bedrock"))
            and not any(qualifier in line for qualifier in ("not support", "does not", "exclude"))
        )
        for line in lines
    )
    passed = passed and pricing_scoped and top_level_scoped
    detail = (
        f"searches={len(successful_searches)}/{len(searches)} successful/attempted, "
        f"parallel_batch={bool(parallel_batches)}, "
        f"pricing_scoped={pricing_scoped}, top_level_scoped={top_level_scoped}, "
        f"durations_ms={[round(e.get('data', {}).get('duration_ms', 0)) for e in searches]}"
    )
    return Result(
        "parallel-research",
        passed,
        detail,
        elapsed,
        session_usage(events),
        output,
        events,
        peak_rss_bytes(run.stderr),
    )


def evaluate_checkpoint(binary: Path, workspace: Path, home: Path, env: dict[str, str]) -> Result:
    protected = (
        workspace / "dataset" / "SWE_BENCH_ISSUE.md",
        workspace / "astropy" / "modeling" / "separable.py",
        workspace / "astropy" / "modeling" / "tests" / "test_separable.py",
    )
    before = {path: path.read_bytes() for path in protected}
    before_files = {path.relative_to(workspace) for path in workspace.rglob("*") if path.is_file()}
    turns = (
        "Inspect dataset/SWE_BENCH_ISSUE.md, astropy/modeling/separable.py, and "
        "astropy/modeling/tests/test_separable.py. Establish the real issue, "
        "defect, regression gap, and next action. Do not modify files.",
        "Continue the analysis without modifying files. If the runtime asks for "
        "a checkpoint, call checkpoint as a standalone tool with a complete "
        "fact-only durable state and keep only the relevant files. Do not put "
        "commands or future actions in the checkpoint. The checkpoint call ends "
        "this turn, so do not combine it with another tool. Put exact issue and "
        "function identifiers in verbatim.",
        "Without rereading or modifying files, state the SWE-bench instance, "
        "affected function, faulty right-matrix behavior, nested model example, "
        "and next regression test.",
    )
    dialog = "\n".join((*turns, "/q", ""))
    run, events, elapsed = run_agent(
        binary,
        workspace,
        home,
        env,
        "checkpoint",
        dialog,
        interactive=True,
        overrides={
            "UAGENT_CONTEXT": "16000",
            "UAGENT_MAX_TOKENS": "4000",
            "UAGENT_CHECKPOINT_MODE": "apply",
            "UAGENT_CHECKPOINT_PCT": os.environ.get("UAGENT_TEST_CHECKPOINT_PCT", "35"),
            "UAGENT_CHECKPOINT_URGENT_PCT": os.environ.get(
                "UAGENT_TEST_CHECKPOINT_URGENT_PCT", "50"
            ),
            "UAGENT_AUTO_COMPACT_PCT": "0",
        },
    )
    output = run.stdout
    lower = output.lower()
    applied = any(event.get("event") == "checkpoint_applied" for event in events)
    after_files = {path.relative_to(workspace) for path in workspace.rglob("*") if path.is_file()}
    changed = [
        str(path.relative_to(workspace))
        for path, content in before.items()
        if not path.exists() or path.read_bytes() != content
    ]
    added = sorted(str(path) for path in after_files - before_files)
    required = (
        SWE_BENCH_INSTANCE.lower() in lower,
        "_cstack" in lower,
        "right" in lower and ("one" in lower or "1" in lower),
        "nested" in lower,
        "test" in lower,
    )
    passed = run.returncode == 0 and applied and all(required) and not changed and not added
    candidate_turns = [
        event.get("data", {}).get("turn")
        for event in events
        if event.get("event") == "checkpoint_candidate"
    ]
    hint_turns = [
        event.get("data", {}).get("turn")
        for event in events
        if event.get("event") == "checkpoint_hint"
    ]
    detail = (
        f"applied={applied}, "
        f"candidate_turns={candidate_turns}, hint_turns={hint_turns}, "
        f"changed={changed}, added={added}"
    )
    return Result(
        "checkpoint-apply",
        passed,
        detail,
        elapsed,
        session_usage(events),
        output,
        events,
        peak_rss_bytes(run.stderr),
    )


def evaluate_checkpoint_500k(
    binary: Path, workspace: Path, home: Path, env: dict[str, str]
) -> Result:
    protected = (
        workspace / "dataset" / "SWE_BENCH_ISSUE.md",
        workspace / "astropy" / "modeling" / "separable.py",
        workspace / "astropy" / "modeling" / "tests" / "test_separable.py",
    )
    before = {path: path.read_bytes() for path in protected}
    facts = (
        f"SWE-bench instance is {SWE_BENCH_INSTANCE}",
        "affected function is _cstack",
        "nested right-hand dependency matrix is overwritten with ones",
        "next regression is rot & (sh1 & sh2)",
    )
    context = real_long_context(workspace)
    turns = (
        "Memorize these durable facts: "
        + "; ".join(facts)
        + ". The following is real source from the pinned Astropy base commit. "
        "Do not use tools. Reply only READY after reading it. Source context: " + context,
        "Continue from the durable facts. If the runtime suggests a checkpoint, "
        "call checkpoint now as a standalone tool with a fact-only state and no "
        "retained files or results. Do not put commands or future actions in the "
        "checkpoint. The checkpoint call ends this turn, so do not combine it "
        "with another tool. Put all four durable fact strings in verbatim.",
        "Without tools, return all four durable facts exactly.",
    )
    run, events, elapsed = run_agent(
        binary,
        workspace,
        home,
        env,
        "checkpoint-500k-live",
        "\n".join((*turns, "/q", "")),
        interactive=True,
        overrides={
            "UAGENT_CONTEXT": "500000",
            "UAGENT_MAX_TOKENS": "1200",
            "UAGENT_CHECKPOINT_MODE": "apply",
            "UAGENT_CHECKPOINT_PCT": os.environ.get("UAGENT_TEST_CHECKPOINT_PCT", "65"),
            "UAGENT_CHECKPOINT_URGENT_PCT": os.environ.get(
                "UAGENT_TEST_CHECKPOINT_URGENT_PCT", "85"
            ),
            "UAGENT_AUTO_COMPACT_PCT": "0",
            "UAGENT_MAX_TURN_COST": os.environ.get("UAGENT_TEST_MAX_TURN_COST", "0.10"),
            "UAGENT_MAX_TURN_SECONDS": "600",
            "UAGENT_FIRST_EVENT_TIMEOUT": "180",
            "UAGENT_STREAM_IDLE_TIMEOUT": "180",
            "UAGENT_REQUEST_TIMEOUT": "480",
        },
        timeout=720,
    )
    output = run.stdout
    normalized_output = output.lower().replace("`", "").replace("*", "")
    fact_checks = (
        SWE_BENCH_INSTANCE.lower() in normalized_output,
        "_cstack" in normalized_output,
        "right-hand dependency matrix" in normalized_output
        and "overwritten with ones" in normalized_output,
        "rot & (sh1 & sh2)" in normalized_output,
    )
    applied = any(event.get("event") == "checkpoint_applied" for event in events)
    hints = [event for event in events if event.get("event") == "checkpoint_hint"]
    responses = raw_response_usage(events)
    peak_prompt = max(
        (int(usage.get("prompt_tokens") or usage.get("input") or 0) for usage in responses),
        default=0,
    )
    non_checkpoint_tools = [
        event
        for event in events
        if event.get("event") == "tool_result" and event.get("data", {}).get("name") != "checkpoint"
    ]
    changed = [
        str(path.relative_to(workspace))
        for path, content in before.items()
        if not path.exists() or path.read_bytes() != content
    ]
    passed = (
        run.returncode == 0
        and applied
        and bool(hints)
        and peak_prompt >= 300000
        and not non_checkpoint_tools
        and not changed
        and all(fact_checks)
    )
    detail = (
        f"applied={applied}, hints={len(hints)}, "
        f"peak_prompt={peak_prompt}, other_tools={len(non_checkpoint_tools)}, "
        f"facts={sum(fact_checks)}/4, "
        f"changed={changed}"
    )
    return Result(
        "checkpoint-500k-live",
        passed,
        detail,
        elapsed,
        session_usage(events),
        output,
        events,
        peak_rss_bytes(run.stderr),
    )


def probe_intervals(workspace: Path) -> tuple[dict[str, tuple[float, float]], bool]:
    path = workspace / "probe.log"
    values: dict[str, dict[str, float]] = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            parts = line.split(",")
            if len(parts) != 3:
                continue
            label, phase, raw = parts
            try:
                values.setdefault(label, {})[phase] = float(raw)
            except ValueError:
                continue
    intervals = {
        label: (phases["start"], phases["end"])
        for label, phases in values.items()
        if "start" in phases and "end" in phases
    }
    if len(intervals) < 2:
        return intervals, False
    ordered = sorted(intervals.values())
    overlap = any(
        left[0] < right[1] and right[0] < left[1]
        for index, left in enumerate(ordered)
        for right in ordered[index + 1 :]
    )
    return intervals, overlap


def evaluate_subagents(binary: Path, workspace: Path, home: Path, env: dict[str, str]) -> Result:
    specifications = {
        "issue": (
            "dataset/SWE_BENCH_ISSUE.md",
            SWE_BENCH_INSTANCE,
            "return the exact instance ID and the two expressions being compared",
        ),
        "code": (
            "astropy/modeling/separable.py",
            "cright[-right.shape[0]:, -right.shape[1]:] = 1",
            "return the exact assignment used for a non-Model right operand in _cstack",
        ),
        "tests": (
            "astropy/modeling/tests/test_separable.py",
            "cm7",
            "return the highest existing compound_models case key",
        ),
    }
    briefs = "\n".join(
        f"- {name}: delegate a standalone task that first runs "
        f"`python3 slow_probe.py {name}` with run timeout=0, then reads "
        f"{path} and {instruction}."
        for name, (path, _, instruction) in specifications.items()
    )
    prompt = f"""Exercise delegation concurrency. In your first action issue
exactly three task calls in the same tool response, each with timeout=1, using
these complete standalone briefs:
{briefs}
The children must perform the slow probe before reading. Keep the parent alive,
collect every background child, and finish with all three exact requested
facts. Do not perform their work in the parent."""
    before = set((home / ".uagent" / "sessions").glob("*.jsonl"))
    run, events, elapsed = run_agent(binary, workspace, home, env, "subagents", prompt, timeout=360)
    after = set((home / ".uagent" / "sessions").glob("*.jsonl"))
    child_traces = [read_events(path) for path in sorted(after - before)]
    intervals, overlap = probe_intervals(workspace)
    output = run.stdout.strip()
    task_events = tool_events(events, "task")
    backgrounded = sum(
        "[backgrounded]" in event.get("data", {}).get("result", "") for event in task_events
    )
    task_batches = [
        event
        for event in events
        if event.get("event") == "tool_batch" and event.get("data", {}).get("calls") == 3
    ]
    parent_parallel = any(event.get("data", {}).get("parallel") is True for event in task_batches)
    passed = (
        run.returncode == 0
        and len(task_events) == 3
        and backgrounded >= 2
        and len(intervals) == 3
        and overlap
        and all(marker in output for _, marker, _ in specifications.values())
        and not parent_parallel
    )
    detail = (
        f"tasks={len(task_events)}, backgrounded={backgrounded}, "
        f"child_traces={len(child_traces)}, probes={len(intervals)}, "
        f"overlap={overlap}, parent_parallel={parent_parallel}"
    )
    return Result(
        "subagent-overlap",
        passed,
        detail,
        elapsed,
        session_usage(events),
        output,
        events,
        peak_rss_bytes(run.stderr),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--binary", type=Path, default=Path("build/uagent"))
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument(
        "--artifacts",
        type=Path,
        help="keep the isolated workspace and traces at this path",
    )
    parser.add_argument(
        "--scenario",
        choices=(
            "analysis",
            "research",
            "checkpoint",
            "checkpoint500k",
            "subagents",
            "all",
        ),
        default="all",
    )
    parser.add_argument(
        "--provider",
        default="default",
        help="preferred OpenRouter provider, or 'default' for router-selected",
    )
    args = parser.parse_args()
    if not args.run:
        parser.print_help()
        print("\nRefusing to spend API credit without --run.")
        return 2

    binary = args.binary.resolve()
    if not binary.is_file():
        print(f"binary not found: {binary}", file=os.sys.stderr)
        return 2
    config = load_config(args.config)
    api_key = (
        os.environ.get("OPENROUTER_API_KEY")
        or config.get("OPENROUTER_API_KEY")
        or os.environ.get("UAGENT_API_KEY")
        or config.get("UAGENT_API_KEY")
    )
    if not api_key:
        print("OPENROUTER_API_KEY is missing.", file=os.sys.stderr)
        return 2

    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
        root_context: Any = contextlib.nullcontext(str(args.artifacts.resolve()))
    else:
        root_context = tempfile.TemporaryDirectory(prefix="uagent-live-workflows-")
    with root_context as temp:
        root = Path(temp)
        workspace = root / "workspace"
        home = root / "home"
        workspace.mkdir()
        home.mkdir()
        write_fixture(workspace)
        env = dict(os.environ)
        env.update(
            {
                "HOME": str(home),
                "UAGENT_BASE_URL": os.environ.get("UAGENT_BASE_URL")
                or config.get("UAGENT_BASE_URL")
                or "https://openrouter.ai/api/v1",
                "UAGENT_MODEL": os.environ.get("UAGENT_MODEL")
                or config.get("UAGENT_MODEL")
                or os.environ.get("OPENROUTER_MODEL")
                or config.get("OPENROUTER_MODEL")
                or DEFAULT_MODEL,
                "UAGENT_API_KEY": api_key,
                "UAGENT_MAX_TOKENS": "1200",
                "UAGENT_MAX_TURN_COST": "0.04",
                "UAGENT_MAX_STEPS": "16",
                "UAGENT_MAX_TOOL_CALLS": "40",
                "UAGENT_MAX_TURN_SECONDS": "240",
                "UAGENT_TOOL_RESULT_CHARS": "6000",
                "UAGENT_TOOL_CONCURRENCY": "3",
                "UAGENT_MAX_BACKGROUND_JOBS": "8",
                "UAGENT_OPENROUTER_FALLBACKS": "1",
                "UAGENT_CHROME_DEVTOOLS": "0",
            }
        )
        if args.provider == "default":
            env.pop("UAGENT_OPENROUTER_PROVIDER", None)
        else:
            env["UAGENT_OPENROUTER_PROVIDER"] = args.provider
        selected = (
            ("analysis", "research", "checkpoint", "subagents")
            if args.scenario == "all"
            else (args.scenario,)
        )
        evaluators = {
            "analysis": evaluate_analysis,
            "research": evaluate_research,
            "checkpoint": evaluate_checkpoint,
            "checkpoint500k": evaluate_checkpoint_500k,
            "subagents": evaluate_subagents,
        }
        results: list[Result] = []
        total_cost = 0.0
        routing = (
            "OpenRouter default" if args.provider == "default" else f"preferred {args.provider}"
        )
        print(
            f"Live µAgent workflows: model={env['UAGENT_MODEL']}, routing={routing}, fallbacks=on"
        )
        for name in selected:
            if total_cost >= MAX_REPORTED_COST:
                print(f"SKIP {name}: reported cost cap reached")
                continue
            result = evaluators[name](binary, workspace, home, env)
            results.append(result)
            cost = float(result.usage.get("cost") or 0)
            total_cost += cost
            cache = int(result.usage.get("cache_read") or 0)
            input_tokens = int(result.usage.get("input") or 0)
            print(
                f"{'PASS' if result.passed else 'FAIL'} {result.name}: "
                f"{result.elapsed:.1f}s, input={input_tokens}, cached={cache}, "
                f"cost=${cost:.6f}, peak_rss={result.peak_rss_bytes / 1048576:.1f}MiB; "
                f"{result.detail}"
            )
            if not result.passed:
                snippet = " ".join(result.output.split())[-500:]
                print(f"  output tail: {snippet}")
        print(f"reported aggregate cost=${total_cost:.6f}")
        if args.artifacts:
            print(f"artifacts={root}")
        return 0 if results and all(result.passed for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
