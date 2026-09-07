#!/usr/bin/env python3
"""Run one generation of µAgent's recursive binary A/B self-improvement.

The executing agent is a native binary, so it can be separated from the source
tree it is asked to improve. One generation therefore looks like this:

    A0 + fresh S0 + I -> candidate source S1 -> gate -> candidate bundle A1
    A0 + fresh S0 + I -> control result       (paired replay)
    A1 + fresh S0 + I -> candidate result
    A0 + fresh S1 + I -> control proposal     (one-generation continuation)
    A1 + fresh S1 + I -> candidate proposal

Only the executor differs inside a pair. Every subject copy is disposable and
byte-identical at the start of its run, every limit is enforced by this
controller rather than by the agent being measured, and the verdict is a
deterministic comparison of externally collected measurements.

This script is the authority for identity, isolation, budgets, gates, the
verdict and the promotion pointer. The agent under test never edits it: the
frozen copy that runs a generation lives outside the writable subject tree, and
a candidate that modifies a protected path is rejected before any comparison.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
from datetime import UTC, datetime
from typing import Any

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from agent_run import (  # noqa: E402
    RunError,
    RunSpec,
    classify_change,
    digest_bytes,
    execute,
    extract_snapshot,
    run_process,
    session_environment,
    tree_digests,
    tree_identity,
    write_snapshot,
)
from live_authority import (  # noqa: E402
    AuthorityError,
    account_result,
    apply_authority,
    load_authority,
)

SCHEMA = "uagent.improvement.generation.v1"
STATE_SCHEMA = "uagent.improvement.state.v1"
CLAIM_SCHEMA = "uagent.improvement.claim.v1"
CLAIM_FILE = ".uagent-improvement.json"
INSTRUCTION_NAME = "INSTRUCTION.md"
CONTROLLER_FILES = ("experiment.py", "agent_run.py", "live_authority.py")
PROTECTED_PATHS = (
    "skills/self-improve/INSTRUCTION.md",
    "skills/self-improve/scripts/experiment.py",
    "skills/self-improve/scripts/agent_run.py",
    "skills/self-improve/scripts/live_authority.py",
    "CMakeLists.txt",
    "CMakePresets.json",
    "pyproject.toml",
    "uv.lock",
    "src/agent/trace.cc",
    "src/core/debug.cc",
    "src/core/sandbox_posix.cc",
)
TEST_PREFIXES = ("tests/", "benchmarks/")
VARIANTS = ("control", "candidate")
STATUSES = (
    "initialized",
    "discovered",
    "gated",
    "replayed",
    "continued",
    "decided",
    "promoted",
    "rolled_back",
)
VERDICTS = ("promote", "reject", "inconclusive", "incomparable")
DIMENSIONS = ("cost_usd", "tokens", "wall_ms", "tool_failures")
MAX_STATE_FILE_BYTES = 1024 * 1024
MAX_COST_USD = 100.0
MAX_RUNS = 32
MAX_PAIRS = 8
MAX_CLAIM_BYTES = 16 * 1024
MAX_COMMAND_CHARS = 4096


class ControllerError(RuntimeError):
    """The controller refuses to continue; the incumbent stays active."""


# --- small io helpers --------------------------------------------------------


def now() -> str:
    return datetime.now(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def private_dir(path: pathlib.Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    path.chmod(0o700)


def atomic_write(path: pathlib.Path, body: bytes, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(handle, "wb") as output:
            output.write(body)
            output.flush()
            os.fchmod(output.fileno(), mode)
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def write_json(path: pathlib.Path, value: Any) -> None:
    atomic_write(path, (json.dumps(value, indent=2, sort_keys=True) + "\n").encode())


def read_json(path: pathlib.Path) -> Any:
    try:
        body = path.read_bytes()
    except FileNotFoundError as error:
        raise ControllerError(f"missing {path}") from error
    if len(body) > MAX_STATE_FILE_BYTES:
        raise ControllerError(f"state file exceeds {MAX_STATE_FILE_BYTES} bytes: {path}")
    try:
        return json.loads(body)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        raise ControllerError(f"invalid JSON in {path}: {error}") from error


def require_number(value: Any, name: str, minimum: float, maximum: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ControllerError(f"{name} must be a number")
    number = float(value)
    if not minimum <= number <= maximum:
        raise ControllerError(f"{name} must be between {minimum:g} and {maximum:g}")
    return number


def parse_command(text: str, name: str) -> list[str]:
    if not text or len(text) > MAX_COMMAND_CHARS:
        raise ControllerError(f"{name} must contain 1..{MAX_COMMAND_CHARS} characters")
    try:
        parts = shlex.split(text)
    except ValueError as error:
        raise ControllerError(f"{name} is not a valid command: {error}") from error
    if not parts:
        raise ControllerError(f"{name} is empty")
    return parts


def controller_revision() -> str:
    """Identity of the pinned controller and verifier logic for a generation."""
    here = pathlib.Path(__file__).resolve().parent
    digests = {name: digest_bytes((here / name).read_bytes()) for name in CONTROLLER_FILES}
    return tree_identity(digests)


# --- state pointer -----------------------------------------------------------


def state_path(root: pathlib.Path) -> pathlib.Path:
    return root / "state.json"


def load_state(root: pathlib.Path) -> dict[str, Any]:
    path = state_path(root)
    if not path.exists():
        return {
            "schema": STATE_SCHEMA,
            "updated_at": now(),
            "active": None,
            "previous": None,
            "history": [],
        }
    state = read_json(path)
    if not isinstance(state, dict) or state.get("schema") != STATE_SCHEMA:
        raise ControllerError(f"unsupported state schema in {path}")
    for name in ("active", "previous", "history"):
        if name not in state:
            raise ControllerError(f"state is missing {name}")
    if not isinstance(state["history"], list):
        raise ControllerError("state.history must be a list")
    return state


def save_state(root: pathlib.Path, state: dict[str, Any]) -> None:
    state["updated_at"] = now()
    write_json(state_path(root), state)


# --- generations -------------------------------------------------------------


def generations_dir(root: pathlib.Path) -> pathlib.Path:
    return root / "generations"


def generation_path(root: pathlib.Path, number: int) -> pathlib.Path:
    return generations_dir(root) / f"{number:04d}"


def next_generation(root: pathlib.Path) -> int:
    existing = [
        int(path.name)
        for path in generations_dir(root).glob("[0-9][0-9][0-9][0-9]")
        if path.is_dir() and path.name.isdigit()
    ]
    return max(existing, default=0) + 1


def resolve_generation(root: pathlib.Path, requested: int | None) -> pathlib.Path:
    number = requested if requested is not None else next_generation(root) - 1
    if number < 1:
        raise ControllerError("no generation exists yet; run init first")
    path = generation_path(root, number)
    if not path.is_dir():
        raise ControllerError(f"generation {number:04d} does not exist")
    return path


def load_manifest(path: pathlib.Path) -> dict[str, Any]:
    manifest = read_json(path / "manifest.json")
    if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA:
        raise ControllerError(f"unsupported generation schema in {path}")
    if manifest.get("status") not in STATUSES:
        raise ControllerError("generation status is invalid")
    if manifest.get("controller_revision") != controller_revision():
        raise ControllerError(
            "the controller changed since this generation started, so its runs are no longer "
            "comparable; start a new generation"
        )
    return manifest


def save_manifest(path: pathlib.Path, manifest: dict[str, Any], status: str | None = None) -> None:
    if status is not None:
        if status not in STATUSES:
            raise ControllerError(f"unknown status {status}")
        manifest["status"] = status
    manifest["updated_at"] = now()
    write_json(path / "manifest.json", manifest)


def require_status(manifest: dict[str, Any], allowed: tuple[str, ...]) -> None:
    if manifest["status"] not in allowed:
        raise ControllerError(
            f"generation status is {manifest['status']}; this step requires one of "
            f"{', '.join(allowed)}"
        )


# --- immutable bundles -------------------------------------------------------


def copy_skills(source: pathlib.Path, destination: pathlib.Path) -> None:
    shutil.copytree(
        source,
        destination,
        ignore=shutil.ignore_patterns("__pycache__", ".*"),
        dirs_exist_ok=True,
    )


def bundle_digests(binary: pathlib.Path, skills: pathlib.Path | None) -> dict[str, str]:
    digests = {"bin/uagent": digest_bytes(binary.read_bytes())}
    if skills is not None and skills.is_dir():
        for path in sorted(skills.rglob("*")):
            relative = path.relative_to(skills)
            if any(part.startswith(".") or part == "__pycache__" for part in relative.parts):
                continue
            if path.is_file():
                digests[f"skills/{relative.as_posix()}"] = digest_bytes(path.read_bytes())
    return digests


def build_bundle(
    root: pathlib.Path, binary: pathlib.Path, skills: pathlib.Path | None
) -> dict[str, Any]:
    """Freeze one runnable version: the executable plus the prompts it ships."""
    binary = binary.expanduser().resolve()
    if not binary.is_file():
        raise ControllerError(f"executable does not exist: {binary}")
    digests = bundle_digests(binary, skills)
    identity = tree_identity(digests)
    bundles = root / "bundles"
    private_dir(bundles)
    destination = bundles / identity
    executable = destination / "bin" / "uagent"
    if not executable.exists():
        staging = pathlib.Path(tempfile.mkdtemp(prefix=".bundle.", dir=bundles))
        try:
            (staging / "bin").mkdir(parents=True)
            shutil.copyfile(binary, staging / "bin" / "uagent")
            (staging / "bin" / "uagent").chmod(0o500)
            if skills is not None and skills.is_dir():
                copy_skills(skills, staging / "skills")
            write_json(
                staging / "bundle.json",
                {
                    "identity": identity,
                    "created_at": now(),
                    "files": digests,
                    "origin": {"binary": str(binary), "skills": str(skills) if skills else None},
                },
            )
            if destination.exists():
                shutil.rmtree(staging, ignore_errors=True)
            else:
                os.replace(staging, destination)
        except Exception:
            shutil.rmtree(staging, ignore_errors=True)
            raise
    return {
        "identity": identity,
        "path": str(executable),
        "skills": str(destination / "skills") if (destination / "skills").is_dir() else None,
        "files": len(digests),
    }


# --- one measured attempt ----------------------------------------------------


def verify_bundle(bundle):
    skills = pathlib.Path(bundle["skills"]) if bundle.get("skills") else None
    if tree_identity(bundle_digests(pathlib.Path(bundle["path"]), skills)) != bundle["identity"]:
        raise ControllerError("immutable executor bundle changed")


def runs_path(path: pathlib.Path) -> pathlib.Path:
    return path / "runs.json"


def load_runs(path: pathlib.Path) -> list[dict[str, Any]]:
    if not runs_path(path).exists():
        return []
    records = read_json(runs_path(path))
    if not isinstance(records, list):
        raise ControllerError("runs.json must be a list")
    return records


def spent_usd(records: list[dict[str, Any]]) -> float:
    return sum(float(record.get("cost_usd") or 0.0) for record in records)


def history_digest(state: dict[str, Any], limit: int = 5) -> list[dict[str, Any]]:
    """The bounded, identical prior evidence both executors are allowed to see."""
    return [
        {
            "generation": entry.get("generation"),
            "verdict": entry.get("verdict"),
            "changed_paths": entry.get("changed_paths", []),
        }
        for entry in state.get("history", [])[-limit:]
    ]


def compose_prompt(manifest: dict[str, Any], history) -> str:
    """The frozen instruction plus run context that is identical within a pair.

    Nothing here names the run's own directory: an absolute path would differ
    between control and candidate and make the two prompts unequal, which is
    exactly the input drift a paired comparison must not have. The subject copy
    is the working directory the executor is launched in.
    """
    instruction = manifest["instruction"]["text"]
    limits = manifest["limits"]
    gates = manifest["gates"]
    context = [
        "## Run context",
        "",
        "Your working directory is a fresh disposable copy of the subject source.",
        "Edit that tree in place. Nothing outside it is yours to change.",
        "",
        "Protected paths, unchanged or the attempt is rejected:",
        *(f"- {name}" for name in manifest["protected_paths"]),
        "",
        "Existing gates that must still pass, run from the workspace root:",
        *(f"- {command}" for command in [gates["build"], *gates["checks"]] if command),
        "",
        f"Write your single claim to {CLAIM_FILE} at the workspace root, as JSON:",
        json.dumps(
            {
                "schema": CLAIM_SCHEMA,
                "hypothesis": "one falsifiable sentence",
                "measurement": "what the check demonstrates",
                "verify_command": "the command that reproduces the claimed gain",
            },
            sort_keys=True,
        ),
        "",
        "The controller reruns verify_command in a clean copy of your tree. A missing, "
        "unreproducible or trivially passing check counts as no validated improvement.",
        "",
        f"Hard limits enforced outside this session: ${limits['max_cost_usd']:.2f}, "
        f"{limits['max_wall_seconds']}s wall clock, {limits['max_model_calls']} model calls, "
        f"{limits['max_tool_calls']} tool calls.",
        "",
        f"Prior generations: {json.dumps(history, sort_keys=True)}",
    ]
    return instruction.rstrip() + "\n\n" + "\n".join(context) + "\n"


def run_attempt(
    _root: pathlib.Path,
    path: pathlib.Path,
    manifest: dict[str, Any],
    *,
    executor: dict[str, Any],
    subject: dict[str, Any],
    label: str,
    phase: str,
    variant: str,
) -> tuple[dict[str, Any], pathlib.Path]:
    """Run one explicitly named bundle against one fresh copy of one subject."""
    verify_bundle(executor)
    verify_bundle(manifest["sandbox"])
    records = load_runs(path)
    if len(records) >= int(manifest["limits"]["max_runs"]):
        raise ControllerError(
            f"generation already used its {manifest['limits']['max_runs']} run allowance"
        )
    declaration = manifest["authority"]["declaration"]
    if any(record.get("budget_breach") or record.get("returncode") for record in records):
        raise ControllerError(
            "a previous run failed or breached its budget; start a new generation"
        )
    cap = float(manifest["limits"]["max_cost_usd"])
    spent = spent_usd(records)
    budget = None
    if declaration["mode"] == "reported-cost":
        budget = cap / int(manifest["limits"]["max_runs"])
        if cap - spent < budget:
            raise ControllerError(f"generation budget of ${cap:.2f} is exhausted")
    work = path / "work" / label
    if work.exists():
        shutil.rmtree(work)
    workspace = work / "source"
    home = work / "home"
    extract_snapshot(pathlib.Path(subject["archive"]), workspace)
    if tree_digests(workspace) != subject["digests"]:
        raise ControllerError("frozen source snapshot changed")
    home.mkdir(parents=True)
    env = {
        "UAGENT_MAX_STEPS": str(manifest["limits"]["max_model_calls"]),
        "UAGENT_MAX_TOOL_CALLS": str(manifest["limits"]["max_tool_calls"]),
        "UAGENT_MAX_TURN_SECONDS": str(manifest["limits"]["max_wall_seconds"]),
    }
    if executor.get("skills"):
        env["UAGENT_SKILL_PATH"] = str(executor["skills"])
    apply_authority(env, declaration)
    # Every member of a pair receives the same per-run limits. The generation
    # ceiling is max_runs times these limits; dollar spend also has its own cap.
    remaining = {
        key: int(manifest["limits"][key])
        for key in ("max_model_calls", "max_tool_calls", "max_tokens")
    }
    for variable, key in (
        ("UAGENT_MAX_STEPS", "max_model_calls"),
        ("UAGENT_MAX_TOOL_CALLS", "max_tool_calls"),
    ):
        env[variable] = str(min(int(env[variable]), remaining[key]))
    env["UAGENT_SANDBOX"] = "0"  # the outer pinned process already confines every write
    env["UAGENT_ADVISOR_MODEL"] = ""
    env["UAGENT_TRUST_PROJECT_CONFIG"] = "0"
    env["UAGENT_SUBAGENT_DEPTH"] = "0"
    env["UAGENT_ADAPT_SYSTEM"] = "0"
    env["UAGENT_INTERNAL_TOOL_ALLOWLIST"] = json.dumps(manifest["tool_allowlist"])
    if manifest.get("config"):
        config = pathlib.Path(manifest["config"]["path"])
        if digest_bytes(config.read_bytes()) != manifest["config"]["sha256"]:
            raise ControllerError("frozen route configuration changed")
        (home / ".uagent").mkdir()
        shutil.copyfile(config, home / ".uagent" / ".config")
    spec = RunSpec(
        binary=pathlib.Path(executor["path"]),
        workspace=workspace,
        home=home,
        route=manifest["cohort"]["route"],
        prompt=compose_prompt(manifest, manifest["history"]),
        trace_path=home / "trace.jsonl",
        budget_usd=budget,
        timeout_seconds=int(manifest["limits"]["max_wall_seconds"]),
        env=env,
        sandbox_binary=pathlib.Path(manifest["sandbox"]["path"]),
        max_model_calls=int(env["UAGENT_MAX_STEPS"]),
        max_tool_calls=int(env["UAGENT_MAX_TOOL_CALLS"]),
        max_tokens=remaining["max_tokens"],
    )
    records.append(
        {
            "label": label,
            "returncode": 125,
            "budget_breach": "run interrupted before accounting completed",
        }
    )
    write_json(runs_path(path), records)
    try:
        result = execute(spec)
    except RunError as error:
        raise ControllerError(str(error)) from error
    breach = result.get("limit_error") or result.get("error")
    try:
        account_result(result, declaration, spent, cap)
    except AuthorityError as error:
        breach = str(error)
    record = {
        "label": label,
        "phase": phase,
        "variant": variant,
        "started_at": now(),
        "executor": executor["identity"],
        "subject": subject["identity"],
        "route": manifest["cohort"]["route"],
        "returncode": result["returncode"],
        "timed_out": result["timed_out"],
        "cost_usd": round(float(result["cost_usd"]), 6),
        "tokens": result["tokens"],
        "wall_ms": result["wall_ms"],
        "model_requests": result["model_requests"],
        "tool_calls": result["tool_calls"],
        "tool_failures": result["tool_failures"],
        "budget_breach": breach,
    }
    records[-1] = record
    write_json(runs_path(path), records)
    return record, workspace


# --- gates -------------------------------------------------------------------


def read_claim(workspace: pathlib.Path) -> tuple[dict[str, Any] | None, str | None]:
    path = workspace / CLAIM_FILE
    if not path.is_file():
        return None, f"no {CLAIM_FILE} was written"
    body = path.read_bytes()
    if len(body) > MAX_CLAIM_BYTES:
        return None, f"{CLAIM_FILE} exceeds {MAX_CLAIM_BYTES} bytes"
    try:
        claim = json.loads(body)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        return None, f"{CLAIM_FILE} is not valid JSON: {error}"
    if not isinstance(claim, dict) or claim.get("schema") != CLAIM_SCHEMA:
        return None, f"{CLAIM_FILE} does not use {CLAIM_SCHEMA}"
    for name in ("hypothesis", "measurement", "verify_command"):
        value = claim.get(name)
        if not isinstance(value, str) or not 1 <= len(value) <= MAX_COMMAND_CHARS:
            return None, f"{CLAIM_FILE}.{name} is missing or invalid"
    return (
        {
            "hypothesis": claim["hypothesis"],
            "measurement": claim["measurement"],
            "verify_command": claim["verify_command"],
        },
        None,
    )


def run_gate(
    command: str, workspace: pathlib.Path, log: pathlib.Path, seconds: int, manifest: dict
) -> dict:
    home = workspace.parent / "gate-home"
    home.mkdir(exist_ok=True)
    spec = RunSpec(
        binary=pathlib.Path(manifest["sandbox"]["path"]),
        workspace=workspace,
        home=home,
        route="",
        prompt="",
        trace_path=home / "trace",
    )
    result = run_process(
        parse_command(command, "gate command"),
        workspace=workspace,
        env=session_environment(spec),
        timeout=seconds,
        sandbox_binary=spec.binary,
        writable_roots=(workspace, home),
    )
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(result.pop("stdout") + result.pop("stderr"), encoding="utf-8")
    return {"command": command, **result, "log": str(log)}


def evaluate_tree(
    manifest: dict[str, Any],
    subject: dict[str, Any],
    workspace: pathlib.Path,
    logs: pathlib.Path,
    *,
    run_gates: bool = True,
) -> dict[str, Any]:
    """Judge one produced tree from the outside: what changed, and does it hold.

    Eligibility and outcome are separate answers. An attempt that touched the
    controller, deleted coverage, crashed or blew a budget is ineligible and can
    never win. An eligible attempt that produced nothing, or only cosmetics,
    simply has no validated improvement to compare.
    """
    reasons: list[str] = []
    change = classify_change(
        subject["digests"],
        workspace,
        test_prefixes=tuple(manifest["test_prefixes"]),
        protected=tuple(manifest["protected_paths"]),
        ignore=(CLAIM_FILE,),
    )
    eligible = True
    if change["touched_protected"]:
        eligible = False
        reasons.append(f"modified protected paths: {', '.join(change['touched_protected'])}")
    if change["weakened_tests"]:
        eligible = False
        reasons.append(f"weakened gates: {', '.join(change['weakened_tests'])}")
    claim, claim_error = read_claim(workspace)
    if claim_error:
        reasons.append(claim_error)
    gates: list[dict[str, Any]] = []
    substantive = change["classification"] == "substantive"
    if not substantive:
        reasons.append(f"change is {change['classification']}")
    if eligible and substantive and claim is not None and run_gates:
        seconds = int(manifest["limits"]["gate_seconds"])
        commands = [manifest["gates"]["build"], *manifest["gates"]["checks"]]
        for index, command in enumerate(command for command in commands if command):
            gates.append(
                run_gate(command, workspace, logs / f"gate-{index}.log", seconds, manifest)
            )
            if gates[-1]["returncode"] != 0:
                reasons.append(f"gate failed: {command}")
                break
        else:
            gates.append(
                run_gate(claim["verify_command"], workspace, logs / "verify.log", seconds, manifest)
            )
            if gates[-1]["returncode"] != 0:
                reasons.append("the claimed measurement did not reproduce")
    gates_passed = bool(gates) and all(
        gate["returncode"] == 0 and not gate["limit_error"] for gate in gates
    )
    comparison_key = None
    if gates_passed:
        # Reproduce the same claimed check on the untouched subject. Only newly
        # added measurement files are carried over; production edits are not.
        with tempfile.TemporaryDirectory(prefix="uagent-counterfactual-") as temporary:
            baseline = pathlib.Path(temporary) / "source"
            extract_snapshot(pathlib.Path(subject["archive"]), baseline)
            measurements = [
                name
                for name in change["added"]
                if name.startswith(tuple(manifest["test_prefixes"]))
            ]
            for name in measurements:
                target = baseline / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(workspace / name, target)
            build = run_gate(
                manifest["gates"]["build"], baseline, logs / "baseline-build.log", seconds, manifest
            )
            check = run_gate(
                claim["verify_command"], baseline, logs / "baseline-verify.log", seconds, manifest
            )
            gates.extend([build, check])
            gates_passed = (
                build["returncode"] == 0
                and check["returncode"] == 1
                and not build["limit_error"]
                and not check["limit_error"]
            )
            if not gates_passed:
                reasons.append(
                    "claimed check must pass on the candidate and fail on the original with exit 1"
                )
            else:
                comparison_key = digest_bytes(
                    (
                        claim["verify_command"]
                        + tree_identity(tree_digests(workspace, measurements))
                    ).encode()
                )
    if gates and not gates_passed:
        eligible = False
    current = tree_digests(workspace)
    current.pop(CLAIM_FILE, None)
    if tree_identity(current) != change["after_identity"]:
        eligible = False
        reasons.append("verification commands modified the candidate source")
    outcome = bool(eligible and substantive and claim is not None and gates_passed)
    return {
        "change": change,
        "claim": claim,
        "comparison_key": comparison_key,
        "gates": gates,
        "eligible": eligible,
        "outcome": outcome,
        "reasons": reasons,
    }


# --- deterministic selection -------------------------------------------------


def mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def summarize(records: list[dict[str, Any]]) -> dict[str, Any]:
    """Fold one variant's paired runs into the vector the verdict compares."""
    reasons: list[str] = []
    eligible_runs = 0
    for record in records:
        blocked = (
            list(record["evaluation"]["reasons"]) if not record["evaluation"]["eligible"] else []
        )
        if record.get("budget_breach"):
            blocked.append(record["budget_breach"])
        if record.get("timed_out"):
            blocked.append("session exceeded its wall-clock limit")
        if record.get("returncode"):
            blocked.append(f"executor exited with {record['returncode']}")
        if blocked:
            reasons.extend(blocked)
        else:
            eligible_runs += 1
    changed: set[str] = set()
    for record in records:
        if record["evaluation"]["outcome"]:
            changed.update(record["evaluation"]["change"]["changed"])
    return {
        "trials": len(records),
        "successes": sum(1 for record in records if record["evaluation"]["outcome"]),
        "eligible_runs": eligible_runs,
        "ineligible_reasons": sorted(set(reasons)),
        "changed_paths": sorted(changed),
        "measurement_keys": sorted(
            {
                record["evaluation"].get("comparison_key")
                for record in records
                if record["evaluation"]["outcome"]
            }
        ),
        "cost_usd": round(mean([float(record["cost_usd"]) for record in records]), 6),
        "tokens": round(mean([float(record["tokens"]) for record in records]), 1),
        "wall_ms": round(mean([float(record["wall_ms"]) for record in records]), 1),
        "tool_failures": round(mean([float(record["tool_failures"]) for record in records]), 3),
    }


def relative_change(control: float, candidate: float) -> float:
    """Percent change of a lower-is-better measurement, candidate against control."""
    if control == 0:
        return 0.0 if candidate == 0 else float("inf")
    return (candidate - control) * 100.0 / control


def compare(
    control: dict[str, Any], candidate: dict[str, Any], tolerances: dict[str, Any]
) -> dict[str, Any]:
    """Conservative Pareto rule over one predeclared measurement vector.

    Nothing here is collapsed into a weighted score: a candidate promotes only
    by winning a comparable dimension outright while regressing none, and two
    valid but unrelated improvements are reported as incomparable instead of
    being ranked by an invented number.
    """
    regression_pct = float(tolerances["regression_pct"])
    gain_pct = float(tolerances["gain_pct"])
    dimensions = {}
    for name in DIMENSIONS:
        change = relative_change(float(control[name]), float(candidate[name]))
        dimensions[name] = {
            "control": control[name],
            "candidate": candidate[name],
            "change_pct": None if change == float("inf") else round(change, 3),
            "regressed": change > regression_pct,
            "improved": change < 0 and change <= -gain_pct,
        }
    reasons = []
    if candidate["eligible_runs"] < candidate["trials"]:
        reasons.append(
            "candidate runs were ineligible: " + "; ".join(candidate["ineligible_reasons"])
        )
        return {"verdict": "reject", "reasons": reasons, "dimensions": dimensions}
    if control["eligible_runs"] < control["trials"]:
        return {
            "verdict": "inconclusive",
            "reasons": ["control run was ineligible"],
            "dimensions": dimensions,
        }
    if candidate["successes"] < control["successes"]:
        reasons.append(
            f"control validated {control['successes']} improvements against the candidate's "
            f"{candidate['successes']}"
        )
        return {"verdict": "reject", "reasons": reasons, "dimensions": dimensions}
    if candidate["successes"] == 0 and control["successes"] == 0:
        reasons.append("neither executor produced a validated improvement")
        return {"verdict": "inconclusive", "reasons": reasons, "dimensions": dimensions}
    regressed = [name for name in DIMENSIONS if dimensions[name]["regressed"]]
    improved = [name for name in DIMENSIONS if dimensions[name]["improved"]]
    if regressed:
        reasons.append(f"candidate regressed beyond {regression_pct:g}%: {', '.join(regressed)}")
        return {"verdict": "reject", "reasons": reasons, "dimensions": dimensions}
    if (
        control["successes"]
        and candidate["successes"]
        and control.get("measurement_keys", control["changed_paths"])
        != candidate.get("measurement_keys", candidate["changed_paths"])
    ):
        return {
            "verdict": "incomparable",
            "reasons": ["different validated changes require a shared frozen measurement"],
            "dimensions": dimensions,
        }
    if candidate["successes"] > control["successes"]:
        reasons.append(
            f"candidate validated {candidate['successes']} improvements against the control's "
            f"{control['successes']}"
        )
        return {"verdict": "promote", "reasons": reasons, "dimensions": dimensions}
    if improved:
        reasons.append(
            f"equal validated outcome with at least {gain_pct:g}% less: {', '.join(improved)}"
        )
        return {"verdict": "promote", "reasons": reasons, "dimensions": dimensions}
    if not set(control["changed_paths"]) & set(candidate["changed_paths"]):
        reasons.append(
            "both executors validated different improvements that these frozen measurements "
            "cannot rank"
        )
        return {"verdict": "incomparable", "reasons": reasons, "dimensions": dimensions}
    reasons.append("no dimension moved past its predeclared tolerance")
    return {"verdict": "inconclusive", "reasons": reasons, "dimensions": dimensions}


def pair_records(pairs: list[dict[str, Any]], variant: str) -> list[dict[str, Any]]:
    return [pair[variant] for pair in pairs]


def phase_comparison(pairs: list[dict[str, Any]], tolerances: dict[str, Any]) -> dict[str, Any]:
    control = summarize(pair_records(pairs, "control"))
    candidate = summarize(pair_records(pairs, "candidate"))
    comparison = compare(control, candidate, tolerances)
    return {"control": control, "candidate": candidate, **comparison}


def decide(
    replay: dict[str, Any], continuation: dict[str, Any] | None, tolerances: dict[str, Any]
) -> dict[str, Any]:
    """Combine the paired replay with the one-generation continuation check."""
    replay_result = phase_comparison(replay["pairs"], tolerances)
    reasons = [f"replay: {reason}" for reason in replay_result["reasons"]]
    if replay_result["verdict"] != "promote":
        return {
            "verdict": replay_result["verdict"],
            "reasons": reasons,
            "replay": replay_result,
            "continuation": None,
        }
    if continuation is None or not continuation.get("pairs"):
        raise ControllerError(
            "the replay favors the candidate, so the one-generation continuation check is "
            "required before a verdict; run `continue` first"
        )
    continuation_result = phase_comparison(continuation["pairs"], tolerances)
    reasons.extend(f"continuation: {reason}" for reason in continuation_result["reasons"])
    verdict = continuation_result["verdict"]
    return {
        "verdict": verdict,
        "reasons": reasons,
        "replay": replay_result,
        "continuation": continuation_result,
    }


# --- commands ----------------------------------------------------------------


def root_path(arguments: argparse.Namespace) -> pathlib.Path:
    return arguments.root.expanduser().resolve()


def snapshot_subject(root: pathlib.Path, source: pathlib.Path) -> dict:
    record = write_snapshot(source, root / "snapshots")
    record["origin"] = str(source)
    record["git"] = git_lineage(source)
    return record


def git_lineage(source: pathlib.Path) -> dict[str, Any]:
    """Git stays the lineage mechanism; the snapshot stays the identity."""
    if not (source / ".git").exists():
        return {"commit": None, "dirty": None}
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=source,
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip()
        status = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=source,
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return {"commit": None, "dirty": None}
    return {"commit": commit, "dirty": bool(status)}


def command_init(arguments: argparse.Namespace) -> dict[str, Any]:
    root = root_path(arguments)
    private_dir(root)
    private_dir(generations_dir(root))
    private_dir(root / "snapshots")
    state = load_state(root)
    prefixes = tuple(arguments.test_prefix or TEST_PREFIXES)
    if arguments.source is not None:
        source = arguments.source.expanduser().resolve()
        if root.is_relative_to(source) or source.is_relative_to(root):
            raise ControllerError("experiment state and source must be separate trees")
        if not source.is_dir():
            raise ControllerError(f"source is not a directory: {source}")
        subject = snapshot_subject(root, source)
    elif state["active"]:
        subject = state["active"]["subject"]
    else:
        raise ControllerError("--source is required until a version has been promoted")
    if arguments.binary is not None:
        skills = arguments.skills.expanduser().resolve() if arguments.skills else None
        executor = build_bundle(root, arguments.binary, skills)
    elif state["active"]:
        executor = state["active"]["executor"]
    else:
        raise ControllerError("--binary is required until a version has been promoted")
    instruction = (
        (arguments.instruction or pathlib.Path(__file__).resolve().parent.parent / INSTRUCTION_NAME)
        .expanduser()
        .resolve()
    )
    try:
        instruction_body = instruction.read_bytes()
    except OSError as error:
        raise ControllerError(f"cannot read the instruction: {error}") from error
    if not 1 <= len(instruction_body) <= 32 * 1024:
        raise ControllerError("the instruction must contain 1..32768 bytes")
    try:
        authority = load_authority(
            arguments.cost_authority.expanduser().resolve(), [arguments.route]
        )
    except AuthorityError as error:
        raise ControllerError(f"route authority is invalid: {error}") from error
    declaration = authority["routes"][arguments.route]
    max_cost = require_number(arguments.max_cost, "max_cost", 0.0, MAX_COST_USD)
    if declaration["mode"] == "reported-cost" and max_cost <= 0:
        raise ControllerError("a reported-cost route requires a positive --max-cost")
    if declaration["mode"] == "non-billable-cheap" and max_cost != 0:
        raise ControllerError("an explicitly non-billable cheap route requires --max-cost 0")
    sandbox = build_bundle(
        root,
        arguments.sandbox_binary
        or arguments.binary
        or pathlib.Path(state["active"]["executor"]["path"]),
        None,
    )
    config = None
    if arguments.config:
        body = arguments.config.expanduser().resolve().read_bytes()
        identity = digest_bytes(body)
        destination = root / "configs" / identity
        atomic_write(destination, body)
        config = {"path": str(destination), "sha256": identity}
    generation = next_generation(root)
    path = generation_path(root, generation)
    manifest = {
        "schema": SCHEMA,
        "generation": generation,
        "created_at": now(),
        "updated_at": now(),
        "status": "initialized",
        "controller_revision": controller_revision(),
        "executor": executor,
        "subject": subject,
        "sandbox": sandbox,
        "config": config,
        "history": history_digest(state),
        "tool_allowlist": [
            "read_path",
            "grep",
            "write_file",
            "edit_file",
            "delete_file",
            "run",
            "scratch",
            "activity",
            "skill",
            "uagent_info",
        ],
        "instruction": {
            "path": str(instruction),
            "sha256": digest_bytes(instruction_body),
            "text": instruction_body.decode("utf-8", errors="strict"),
        },
        "cohort": {
            "route": arguments.route + (":" + arguments.effort if arguments.effort else ""),
            "effort": arguments.effort,
        },
        "authority": {
            "sha256": authority["sha256"],
            "mode": declaration["mode"],
            "declaration": declaration,
        },
        "limits": {
            "max_cost_usd": max_cost,
            "max_wall_seconds": int(
                require_number(arguments.max_wall_seconds, "max_wall_seconds", 30, 86_400)
            ),
            "max_model_calls": int(
                require_number(arguments.max_model_calls, "max_model_calls", 1, 1000)
            ),
            "max_tool_calls": int(
                require_number(arguments.max_tool_calls, "max_tool_calls", 1, 5000)
            ),
            "max_tokens": int(require_number(arguments.max_tokens, "max_tokens", 1, 10_000_000)),
            "max_runs": int(require_number(arguments.max_runs, "max_runs", 1, MAX_RUNS)),
            "gate_seconds": int(require_number(arguments.gate_seconds, "gate_seconds", 10, 86_400)),
        },
        "gates": {
            "build": arguments.build_command or "",
            "checks": list(arguments.gate or []),
            "artifact": arguments.artifact,
        },
        "tolerances": {
            "regression_pct": require_number(
                arguments.max_regression_pct, "max_regression_pct", 0.0, 100.0
            ),
            "gain_pct": require_number(arguments.min_gain_pct, "min_gain_pct", 0.0, 100.0),
        },
        "protected_paths": sorted(
            set(PROTECTED_PATHS)
            | set(arguments.protected or [])
            | {name for name in subject["digests"] if name.startswith(".github/")}
        ),
        "test_prefixes": list(prefixes),
    }
    if not manifest["gates"]["build"] or not manifest["gates"]["checks"]:
        raise ControllerError("a build command and at least one existing gate are required")
    artifact = pathlib.Path(manifest["gates"]["artifact"])
    if artifact.is_absolute() or ".." in artifact.parts:
        raise ControllerError("artifact must stay inside the candidate workspace")
    if declaration["mode"] == "non-billable-cheap":
        manifest["limits"]["max_runs"] = min(
            manifest["limits"]["max_runs"], declaration["limits"]["max_sessions"]
        )
        manifest["limits"]["max_wall_seconds"] = min(
            manifest["limits"]["max_wall_seconds"], declaration["limits"]["max_session_seconds"]
        )
    for command in [manifest["gates"]["build"], *manifest["gates"]["checks"]]:
        if command:
            parse_command(command, "gate command")
    if path.exists():
        raise ControllerError(f"generation already exists: {path}")
    private_dir(path)
    pinned = path / "controller"
    private_dir(pinned)
    for name in CONTROLLER_FILES:
        shutil.copyfile(pathlib.Path(__file__).resolve().parent / name, pinned / name)
    save_manifest(path, manifest)
    if not state["active"]:
        state["active"] = {
            "generation": 0,
            "executor": executor,
            "subject": subject,
            "promoted_at": now(),
        }
        save_state(root, state)
    return {
        "generation": generation,
        "path": str(path),
        "executor": executor["identity"],
        "subject": subject["identity"],
        "instruction_sha256": manifest["instruction"]["sha256"],
        "authority_mode": declaration["mode"],
        "controller": str(pinned / "experiment.py"),
        "next": "discover",
    }


def command_discover(arguments: argparse.Namespace) -> dict[str, Any]:
    root = root_path(arguments)
    path = resolve_generation(root, arguments.generation)
    manifest = load_manifest(path)
    require_status(manifest, ("initialized",))
    record, workspace = run_attempt(
        root,
        path,
        manifest,
        executor=manifest["executor"],
        subject=manifest["subject"],
        label="discovery",
        phase="discovery",
        variant="candidate",
    )
    write_json(
        path / "discovery.json",
        {"schema": SCHEMA, "run": record, "workspace": str(workspace)},
    )
    save_manifest(path, manifest, "discovered")
    return {"generation": manifest["generation"], "run": record, "next": "gate"}


def reject_generation(
    path: pathlib.Path, manifest: dict[str, Any], reasons: list[str]
) -> dict[str, Any]:
    verdict = {
        "schema": SCHEMA,
        "generation": manifest["generation"],
        "decided_at": now(),
        "verdict": "reject",
        "reasons": reasons,
        "replay": None,
        "continuation": None,
    }
    write_json(path / "verdict.json", verdict)
    save_manifest(path, manifest, "decided")
    return verdict


def command_gate(arguments: argparse.Namespace) -> dict[str, Any]:
    root = root_path(arguments)
    path = resolve_generation(root, arguments.generation)
    manifest = load_manifest(path)
    require_status(manifest, ("discovered",))
    discovery = read_json(path / "discovery.json")
    workspace = pathlib.Path(discovery["workspace"])
    if not workspace.is_dir():
        raise ControllerError(f"the discovery workspace is missing: {workspace}")
    run = discovery["run"]
    blocking = []
    if run.get("budget_breach"):
        blocking.append(run["budget_breach"])
    if run.get("timed_out"):
        blocking.append("the discovery session exceeded its wall-clock limit")
    if run.get("returncode"):
        blocking.append(f"the discovery session exited with {run['returncode']}")
    evaluation = evaluate_tree(manifest, manifest["subject"], workspace, path / "logs" / "gate")
    write_json(
        path / "gate.json",
        {"schema": SCHEMA, "evaluation": evaluation, "blocking": blocking},
    )
    if blocking or not evaluation["outcome"]:
        return reject_generation(
            path, manifest, [f"discovery: {reason}" for reason in blocking + evaluation["reasons"]]
        )
    (workspace / CLAIM_FILE).unlink(missing_ok=True)
    subject = snapshot_subject(root, workspace)
    artifact = workspace / manifest["gates"]["artifact"]
    if not artifact.is_file():
        return reject_generation(
            path,
            manifest,
            [f"the gated build produced no artifact at {manifest['gates']['artifact']}"],
        )
    skills = workspace / "skills"
    bundle = build_bundle(root, artifact, skills if skills.is_dir() else None)
    write_json(
        path / "gate.json",
        {
            "schema": SCHEMA,
            "evaluation": evaluation,
            "blocking": blocking,
            "candidate_bundle": bundle,
            "candidate_subject": subject,
        },
    )
    save_manifest(path, manifest, "gated")
    return {
        "generation": manifest["generation"],
        "claim": evaluation["claim"],
        "changed": evaluation["change"]["changed"],
        "candidate_bundle": bundle["identity"],
        "candidate_subject": subject["identity"],
        "next": "replay",
    }


def run_phase(arguments: argparse.Namespace, phase: str) -> dict[str, Any]:
    """Run paired trials in which only the executor differs."""
    root = root_path(arguments)
    path = resolve_generation(root, arguments.generation)
    manifest = load_manifest(path)
    require_status(manifest, ("gated", "replayed", "continued"))
    gate = read_json(path / "gate.json")
    if "candidate_bundle" not in gate:
        raise ControllerError("the candidate never passed its gate; there is nothing to compare")
    subject = manifest["subject"] if phase == "replay" else gate["candidate_subject"]
    executors = {"control": manifest["executor"], "candidate": gate["candidate_bundle"]}
    pairs_requested = int(require_number(arguments.pairs, "pairs", 1, MAX_PAIRS))
    file = path / f"{phase}.json"
    report = (
        read_json(file)
        if file.exists()
        else {"schema": SCHEMA, "phase": phase, "subject": subject["identity"], "pairs": []}
    )
    for _ in range(pairs_requested):
        number = len(report["pairs"]) + 1
        if number > MAX_PAIRS:
            raise ControllerError(f"{phase} already ran {MAX_PAIRS} pairs")
        pair: dict[str, Any] = {"pair": number}
        for variant in VARIANTS:
            label = f"{phase}-{number}-{variant}"
            record, workspace = run_attempt(
                root,
                path,
                manifest,
                executor=executors[variant],
                subject=subject,
                label=label,
                phase=phase,
                variant=variant,
            )
            record["evaluation"] = evaluate_tree(
                manifest, subject, workspace, path / "logs" / label
            )
            pair[variant] = record
            if not arguments.keep_workspaces:
                shutil.rmtree(path / "work" / label, ignore_errors=True)
        report["pairs"].append(pair)
        write_json(file, report)
    save_manifest(path, manifest, "replayed" if phase == "replay" else "continued")
    return {
        "generation": manifest["generation"],
        "phase": phase,
        "pairs": len(report["pairs"]),
        "summary": phase_comparison(report["pairs"], manifest["tolerances"]),
        "next": "continue" if phase == "replay" else "verdict",
    }


def command_replay(arguments: argparse.Namespace) -> dict[str, Any]:
    return run_phase(arguments, "replay")


def command_continue(arguments: argparse.Namespace) -> dict[str, Any]:
    return run_phase(arguments, "continuation")


def command_verdict(arguments: argparse.Namespace) -> dict[str, Any]:
    root = root_path(arguments)
    path = resolve_generation(root, arguments.generation)
    manifest = load_manifest(path)
    require_status(manifest, ("replayed", "continued"))
    replay = read_json(path / "replay.json")
    continuation_file = path / "continuation.json"
    continuation = read_json(continuation_file) if continuation_file.exists() else None
    decision = decide(replay, continuation, manifest["tolerances"])
    verdict = {
        "schema": SCHEMA,
        "generation": manifest["generation"],
        "decided_at": now(),
        "tolerances": manifest["tolerances"],
        **decision,
    }
    write_json(path / "verdict.json", verdict)
    save_manifest(path, manifest, "decided")
    return verdict


def command_promote(arguments: argparse.Namespace) -> dict[str, Any]:
    root = root_path(arguments)
    path = resolve_generation(root, arguments.generation)
    manifest = load_manifest(path)
    require_status(manifest, ("decided",))
    if not arguments.approve:
        raise ControllerError("promotion requires an explicit --approve")
    verdict = read_json(path / "verdict.json")
    if verdict.get("verdict") != "promote":
        raise ControllerError(
            f"the verdict is {verdict.get('verdict')}; the incumbent version stays active"
        )
    gate = read_json(path / "gate.json")
    state = load_state(root)
    if (
        state["active"]["executor"]["identity"] != manifest["executor"]["identity"]
        or state["active"]["subject"]["identity"] != manifest["subject"]["identity"]
    ):
        raise ControllerError("active version changed since this generation began")
    verify_bundle(gate["candidate_bundle"])
    state["previous"] = state["active"]
    state["active"] = {
        "generation": manifest["generation"],
        "executor": gate["candidate_bundle"],
        "subject": gate["candidate_subject"],
        "promoted_at": now(),
    }
    state["history"].append(
        {
            "generation": manifest["generation"],
            "verdict": "promote",
            "decided_at": verdict["decided_at"],
            "executor": gate["candidate_bundle"]["identity"],
            "subject": gate["candidate_subject"]["identity"],
            "changed_paths": gate["evaluation"]["change"]["changed"][:20],
        }
    )
    save_state(root, state)
    promotion = {
        "schema": SCHEMA,
        "promoted_at": state["active"]["promoted_at"],
        "generation": manifest["generation"],
        "executor": gate["candidate_bundle"]["identity"],
        "subject": gate["candidate_subject"]["identity"],
        "previous": (state["previous"] or {}).get("executor", {}).get("identity"),
    }
    write_json(path / "promotion.json", promotion)
    save_manifest(path, manifest, "promoted")
    return promotion


def command_rollback(arguments: argparse.Namespace) -> dict[str, Any]:
    root = root_path(arguments)
    state = load_state(root)
    if not state.get("previous"):
        raise ControllerError("there is no previous version to restore")
    restored = state["previous"]
    superseded = state["active"]
    state["active"] = restored
    state["previous"] = superseded
    state["history"].append(
        {
            "generation": (superseded or {}).get("generation"),
            "verdict": "rolled_back",
            "decided_at": now(),
            "executor": (restored or {}).get("executor", {}).get("identity"),
            "subject": (restored or {}).get("subject", {}).get("identity"),
            "changed_paths": [],
        }
    )
    save_state(root, state)
    generation = (superseded or {}).get("generation")
    if generation:
        path = generation_path(root, int(generation))
        if path.is_dir():
            manifest = load_manifest(path)
            save_manifest(path, manifest, "rolled_back")
    return {
        "active": {
            "generation": restored.get("generation"),
            "executor": restored["executor"]["identity"],
            "subject": restored["subject"]["identity"],
            "path": restored["executor"]["path"],
        },
        "rolled_back": generation,
    }


def command_status(arguments: argparse.Namespace) -> dict[str, Any]:
    root = root_path(arguments)
    state = load_state(root)
    generations = []
    if generations_dir(root).is_dir():
        for path in sorted(generations_dir(root).glob("[0-9][0-9][0-9][0-9]")):
            manifest = read_json(path / "manifest.json")
            entry = {
                "generation": manifest.get("generation"),
                "status": manifest.get("status"),
                "executor": manifest.get("executor", {}).get("identity"),
                "subject": manifest.get("subject", {}).get("identity"),
                "spent_usd": round(spent_usd(load_runs(path)), 6),
                "runs": len(load_runs(path)),
            }
            if (path / "verdict.json").exists():
                entry["verdict"] = read_json(path / "verdict.json").get("verdict")
            generations.append(entry)
    active = state.get("active") or {}
    return {
        "root": str(root),
        "active": {
            "generation": active.get("generation"),
            "executor": active.get("executor", {}).get("identity"),
            "path": active.get("executor", {}).get("path"),
            "subject": active.get("subject", {}).get("identity"),
        }
        if active
        else None,
        "rollback_available": bool(state.get("previous")),
        "generations": generations,
    }


# --- command line ------------------------------------------------------------


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path("~/.uagent/improve"),
        help="private state root (default: ~/.uagent/improve)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    initialize = subparsers.add_parser("init", help="snapshot the incumbent and open a generation")
    initialize.add_argument("--source", type=pathlib.Path)
    initialize.add_argument("--binary", type=pathlib.Path)
    initialize.add_argument("--skills", type=pathlib.Path)
    initialize.add_argument(
        "--sandbox-binary", type=pathlib.Path, help="trusted uagent providing the outer sandbox"
    )
    initialize.add_argument(
        "--config", type=pathlib.Path, help="explicit route configuration frozen for every trial"
    )
    initialize.add_argument("--max-tokens", type=int, default=100000)
    initialize.add_argument("--instruction", type=pathlib.Path)
    initialize.add_argument("--route", required=True)
    initialize.add_argument("--effort", default="")
    initialize.add_argument("--cost-authority", type=pathlib.Path, required=True)
    initialize.add_argument("--max-cost", type=float, required=True)
    initialize.add_argument("--max-wall-seconds", type=int, default=1800)
    initialize.add_argument("--max-model-calls", type=int, default=80)
    initialize.add_argument("--max-tool-calls", type=int, default=200)
    initialize.add_argument("--max-runs", type=int, default=8)
    initialize.add_argument("--gate-seconds", type=int, default=1800)
    initialize.add_argument("--build-command", default="")
    initialize.add_argument("--gate", action="append", default=[])
    initialize.add_argument("--artifact", required=True)
    initialize.add_argument("--protected", action="append")
    initialize.add_argument("--test-prefix", action="append")
    initialize.add_argument("--max-regression-pct", type=float, default=10.0)
    initialize.add_argument("--min-gain-pct", type=float, default=10.0)
    initialize.set_defaults(handler=command_init)

    for name, handler, help_text in (
        ("discover", command_discover, "run the incumbent against a fresh subject copy"),
        ("gate", command_gate, "verify the candidate source and build its bundle"),
        ("verdict", command_verdict, "compute the deterministic selection verdict"),
    ):
        sub = subparsers.add_parser(name, help=help_text)
        sub.add_argument("--generation", type=int)
        sub.set_defaults(handler=handler)

    for name, handler, help_text in (
        ("replay", command_replay, "paired A/B trials on the original subject"),
        ("continue", command_continue, "paired A/B trials on the candidate subject"),
    ):
        sub = subparsers.add_parser(name, help=help_text)
        sub.add_argument("--generation", type=int)
        sub.add_argument("--pairs", type=int, default=1)
        sub.add_argument("--keep-workspaces", action="store_true")
        sub.set_defaults(handler=handler)

    promote = subparsers.add_parser("promote", help="advance the version pointer")
    promote.add_argument("--generation", type=int)
    promote.add_argument("--approve", action="store_true")
    promote.set_defaults(handler=command_promote)

    rollback = subparsers.add_parser("rollback", help="restore the previous version pointer")
    rollback.set_defaults(handler=command_rollback)

    status = subparsers.add_parser("status", help="report the pointer and every generation")
    status.set_defaults(handler=command_status)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_args(argv)
    try:
        root = root_path(arguments)
        private_dir(root)
        with (root / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            result = arguments.handler(arguments)
    except (ControllerError, RunError, OSError, ValueError) as error:
        print(json.dumps({"error": str(error)}, indent=2, sort_keys=True))
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
