#!/usr/bin/env python3
"""Shared primitives for running a µAgent binary and measuring what it did.

`benchmarks/eval.py`, `benchmarks/audit.py` and the self-improvement controller
all need the same four things: build a measured command, read a `--debug`
trace, fold that trace into privacy-safe aggregates, and launch one explicitly
selected executable against a disposable workspace. They live here so the
installed skill can drive a generation without a source checkout, and so the
evaluation harnesses keep exactly one implementation.

Nothing in this module resolves a binary by itself. A caller always names the
executable path it means to measure; there is no fallback to the running
process.
"""

from __future__ import annotations

import collections
import hashlib
import json
import os
import re
import selectors
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

MAX_SNAPSHOT_BYTES = 256 * 1024 * 1024
MAX_SNAPSHOT_FILES = 20_000
SNAPSHOT_EXCLUDE = (".git", "build", "__pycache__", ".venv", "node_modules", ".uagent")
DOCUMENTATION_SUFFIXES = (".md", ".txt", ".rst")


class RunError(RuntimeError):
    """A measured run could not be prepared or executed."""


# --- commands and traces -----------------------------------------------------


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


def read_trace(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    records = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(record, dict):
            records.append(record)
    return records


def trace_metrics(
    records: list[dict[str, Any]],
    *,
    provenance_fn: Callable[[Any], Any] | None = None,
    cohort_fn: Callable[[Any], Any] | None = None,
) -> dict[str, Any]:
    """Reconstruct per-request context and privacy-safe trajectory aggregates.

    `provenance_fn`/`cohort_fn` stay injectable because provenance redaction
    lives with the session-journal reader in `benchmarks/`, which an installed
    skill cannot import.
    """
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
            raw = data.get("provenance")
            provenance = provenance_fn(raw) if provenance_fn else None
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
                for field_name in (
                    "input",
                    "output",
                    "cache_read",
                    "cache_write",
                    "reasoning",
                    "web_searches",
                ):
                    usage[field_name] += int(turn_usage.get(field_name) or 0)
                usage["cost"] += float(turn_usage.get("cost") or 0)
                if turn_usage.get("cost_reported"):
                    usage["cost_reported_turns"] += 1
        elif name == "compact_end" and data.get("outcome") == "ok":
            compactions += 1
    no_action = max(len([count for count in batches[:-1] if count == 0]), 0)
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
        "cohort": cohort_fn(provenance) if cohort_fn else "",
        "route": route,
    }


# --- one measured session ----------------------------------------------------


@dataclass(frozen=True)
class RunSpec:
    """Everything that makes one agent session reproducible and bounded."""

    binary: Path
    workspace: Path
    home: Path
    route: str
    prompt: str
    trace_path: Path
    budget_usd: float | None = None
    timeout_seconds: int = 900
    env: dict[str, str] = field(default_factory=dict)
    extra_arguments: tuple[str, ...] = ()
    sandbox_binary: Path | None = None
    max_model_calls: int = 0
    max_tool_calls: int = 0
    max_tokens: int = 0

    def command(self) -> list[str]:
        cli = [
            "--json",
            "--no-memory",
            "--yolo",
            f"--debug={self.trace_path}",
            "--model",
            self.route,
        ]
        if self.budget_usd is not None:
            cli.extend(["--budget", f"{self.budget_usd:.6f}"])
        if self.max_tokens:
            cli.extend(["--token-budget", str(self.max_tokens)])
        cli.extend(self.extra_arguments)
        cli.extend(["-p", self.prompt])
        return measured_command(self.binary, cli)


def session_environment(spec: RunSpec) -> dict[str, str]:
    allowed = {
        "PATH",
        "LANG",
        "LC_ALL",
        "TZ",
        "CC",
        "CXX",
        "SDKROOT",
        "DEVELOPER_DIR",
        "CMAKE_BUILD_PARALLEL_LEVEL",
    }
    env = {key: value for key, value in os.environ.items() if key in allowed}
    scratch = spec.home / "tmp"
    scratch.mkdir(parents=True, exist_ok=True)
    env.update(
        {
            "HOME": str(spec.home),
            "TMPDIR": str(scratch),
            "UAGENT_MEMORY": "0",
            "UAGENT_MEMORY_GENERATE": "0",
        }
    )
    env.update(spec.env)
    return env


def run_process(argv, *, workspace, env, timeout, sandbox_binary, writable_roots, monitor=None):
    """One bounded process group, shared by executor sessions and verifier commands."""
    # Native startup and shell redirection need device files, as in the runtime
    # sandbox. Do not grant /tmp: authoritative experiment state may live there.
    roots = sorted({"/dev", *(str(path.resolve()) for path in writable_roots)})
    command = [
        str(sandbox_binary),
        "--sandbox-child",
        "net=1",
        f"roots={len(roots)}",
        *roots,
        "--",
        *argv,
    ]
    started = time.monotonic()
    reason = None
    output = {"stdout": bytearray(), "stderr": bytearray()}
    with subprocess.Popen(
        command,
        cwd=workspace,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    ) as process:

        def kill_group():
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            except PermissionError:
                # macOS returns EPERM for a group containing only an unreaped
                # zombie. Reap our exited child, then retry so live descendants
                # still receive SIGKILL and real permission failures propagate.
                if sys.platform != "darwin" or process.poll() is None:
                    raise
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ, "stdout")
                selector.register(process.stderr, selectors.EVENT_READ, "stderr")
                while selector.get_map() or process.poll() is None:
                    if time.monotonic() - started >= timeout:
                        reason = "wall-clock limit exceeded"
                    if monitor and not reason:
                        reason = monitor()
                    if reason:
                        kill_group()
                        break
                    for key, _ in selector.select(0.05):
                        data = os.read(key.fileobj.fileno(), 65536)
                        if not data:
                            selector.unregister(key.fileobj)
                        elif len(output[key.data]) + len(data) > 2 * 1024 * 1024:
                            reason = "process output limit exceeded"
                        else:
                            output[key.data].extend(data)
                process.wait(timeout=5)
        finally:
            # This also closes background descendants after a successful parent exit.
            kill_group()
    return {
        "returncode": process.returncode,
        "timed_out": reason == "wall-clock limit exceeded",
        "limit_error": reason,
        "elapsed_seconds": round(time.monotonic() - started, 3),
        **{key: bytes(value).decode(errors="replace") for key, value in output.items()},
    }


def execute(spec: RunSpec) -> dict[str, Any]:
    """Run the recorded executor under an independent, pinned OS sandbox."""
    if not spec.binary.is_file() or not os.access(spec.binary, os.X_OK):
        raise RunError(f"executor is not executable: {spec.binary}")
    if spec.sandbox_binary is None:
        raise RunError("a pinned sandbox binary is required")
    spec.workspace.mkdir(parents=True, exist_ok=True)
    spec.home.mkdir(parents=True, exist_ok=True)
    spec.trace_path.parent.mkdir(parents=True, exist_ok=True)
    spec.trace_path.unlink(missing_ok=True)

    def monitor():
        if spec.trace_path.exists() and spec.trace_path.stat().st_size > 16 * 1024 * 1024:
            return "trace limit exceeded"
        metrics = trace_metrics(read_trace(spec.trace_path))
        for field_name, limit in (
            ("model_requests", spec.max_model_calls),
            ("tool_calls", spec.max_tool_calls),
        ):
            if limit and metrics[field_name] > limit:
                return f"{field_name} limit exceeded"
        tokens = metrics["usage"]["input"] + metrics["usage"]["output"]
        if spec.max_tokens and tokens > spec.max_tokens:
            return "token limit exceeded"
        return None

    process = run_process(
        spec.command(),
        workspace=spec.workspace,
        env=session_environment(spec),
        timeout=spec.timeout_seconds,
        sandbox_binary=spec.sandbox_binary,
        writable_roots=(spec.workspace, spec.home),
        monitor=monitor,
    )
    for stream in ("stdout", "stderr"):
        log = spec.home / f"executor.{stream}.log"
        log.write_text(process[stream], encoding="utf-8")
        log.chmod(0o600)
    try:
        envelope = json.loads(process["stdout"])
    except (json.JSONDecodeError, TypeError):
        envelope = {}
    metrics = trace_metrics(read_trace(spec.trace_path))
    usage = dict(metrics["usage"])
    complete = metrics["events"]["turn_end"] > 0 and metrics["model_requests"] > 0
    return {
        "route": spec.route,
        "returncode": process["returncode"],
        "timed_out": process["timed_out"],
        "limit_error": process["limit_error"] or monitor(),
        "elapsed_seconds": process["elapsed_seconds"],
        "wall_ms": int(process["elapsed_seconds"] * 1000),
        "peak_rss_bytes": peak_rss(process["stderr"]),
        "model_requests": metrics["model_requests"],
        "tool_calls": metrics["tool_calls"],
        "tool_failures": metrics["failed_calls"],
        "unrecovered_tool_failures": metrics["unrecovered_failed_calls"],
        "tokens": int(usage.get("input", 0)) + int(usage.get("output", 0)),
        "output_tokens": int(usage.get("output", 0)),
        "usage": usage,
        "cost_usd": float(usage.get("cost", 0)),
        "cost_reported": bool(usage.get("cost_reported_turns")),
        "error": None
        if complete and isinstance(envelope, dict)
        else "missing complete session telemetry",
    }


# --- frozen source snapshots -------------------------------------------------


def source_files(root: Path) -> list[str]:
    """List the tracked-ish files of a source tree, newest state included.

    Git decides what belongs to the tree when the directory is a checkout, so
    ignored build output never enters a snapshot. A plain directory (the
    hermetic tests use one) falls back to a walk with the same exclusions.
    """
    if (root / ".git").exists():
        try:
            listing = subprocess.run(
                ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                cwd=root,
                text=True,
                capture_output=True,
                check=True,
            ).stdout
            names = [name for name in listing.split("\0") if name]
            return sorted(name for name in names if (root / name).is_file())
        except (OSError, subprocess.CalledProcessError) as error:
            raise RunError(f"cannot list source files in {root}: {error}") from error
    names = []
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if any(part in SNAPSHOT_EXCLUDE for part in relative.parts):
            continue
        if path.is_file() and not path.is_symlink():
            names.append(relative.as_posix())
    return sorted(names)


def digest_bytes(body: bytes) -> str:
    return hashlib.sha256(body).hexdigest()


def tree_digests(root: Path, names: list[str] | None = None) -> dict[str, str]:
    """Map every source path to its content digest plus its executable bit."""
    digests = {}
    for name in names if names is not None else source_files(root):
        path = root / name
        try:
            body = path.read_bytes()
        except OSError as error:
            raise RunError(f"cannot read {path}: {error}") from error
        mode = "x" if os.access(path, os.X_OK) else "-"
        digests[name] = f"{mode}{digest_bytes(body)}"
    return digests


def tree_identity(digests: dict[str, str]) -> str:
    """One stable identity for a set of files, independent of timestamps."""
    payload = "\n".join(f"{name} {digest}" for name, digest in sorted(digests.items()))
    return digest_bytes(payload.encode())


def write_snapshot(root: Path, directory: Path) -> dict[str, Any]:
    """Freeze a source tree into a deterministic tar named by its identity."""
    names = source_files(root)
    if any((root / name).is_symlink() for name in names):
        raise RunError("source snapshots cannot contain symlinks")
    if len(names) > MAX_SNAPSHOT_FILES:
        raise RunError(f"source tree has {len(names)} files, above {MAX_SNAPSHOT_FILES}")
    total = sum((root / name).stat().st_size for name in names)
    if total > MAX_SNAPSHOT_BYTES:
        raise RunError(f"source tree is {total} bytes, above {MAX_SNAPSHOT_BYTES}")
    digests = tree_digests(root, names)
    identity = tree_identity(digests)
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / f"{identity}.tar"
    if not destination.exists():
        handle, temporary = tempfile.mkstemp(prefix=".snapshot.", dir=directory)
        os.close(handle)
        try:
            with tarfile.open(temporary, "w") as archive:
                for name in names:
                    info = archive.gettarinfo(root / name, arcname=name)
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mtime = 0
                    info.mode = 0o755 if digests[name].startswith("x") else 0o644
                    with open(root / name, "rb") as body:
                        archive.addfile(info, body)
            os.replace(temporary, destination)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    return {
        "identity": identity,
        "files": len(names),
        "bytes": total,
        "archive": str(destination),
        "digests": digests,
    }


def extract_snapshot(archive: Path, destination: Path) -> None:
    """Materialize one disposable copy of a frozen source tree."""
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    with tarfile.open(archive, "r") as tar:
        for member in tar.getmembers():
            name = Path(member.name)
            if name.is_absolute() or ".." in name.parts:
                raise RunError(f"unsafe snapshot member {member.name!r}")
            if member.issym() or member.islnk() or member.isdev():
                raise RunError(f"unsupported snapshot member {member.name!r}")
        tar.extractall(destination, filter="data")


# --- what a run changed ------------------------------------------------------


def is_test_path(name: str, test_prefixes: tuple[str, ...]) -> bool:
    return name.startswith(test_prefixes) or "_test." in name or Path(name).name.startswith("test_")


def classify_change(
    before: dict[str, str],
    after_root: Path,
    *,
    test_prefixes: tuple[str, ...],
    protected: tuple[str, ...],
    ignore: tuple[str, ...] = (),
) -> dict[str, Any]:
    """Describe a produced tree against the frozen tree it started from.

    The verdict never trusts the candidate's own account of its patch: added,
    removed and rewritten paths are read back off disk, weakened test files are
    detected by changed content, and a documentation-only or
    whitespace-only patch is reported as cosmetic rather than as work done.
    """
    after = tree_digests(after_root)
    for name in ignore:
        before = {key: value for key, value in before.items() if key != name}
        after = {key: value for key, value in after.items() if key != name}
    added = sorted(set(after) - set(before))
    removed = sorted(set(before) - set(after))
    modified = sorted(name for name in set(before) & set(after) if before[name] != after[name])
    changed = sorted(added + removed + modified)
    touched_protected = sorted(name for name in changed if name in protected)

    # Existing verifier inputs are frozen byte-for-byte. New focused checks may
    # be added, but line counts cannot establish that a rewritten test is safe.
    weakened = [name for name in removed + modified if is_test_path(name, test_prefixes)]
    substantive = any(not name.endswith(DOCUMENTATION_SUFFIXES) for name in changed)
    classification = "none"
    if changed:
        classification = "substantive" if substantive else "cosmetic"
    return {
        "added": added,
        "removed": removed,
        "modified": modified,
        "changed": changed,
        "classification": classification,
        "weakened_tests": sorted(set(weakened)),
        "touched_protected": touched_protected,
        "after_identity": tree_identity(after),
    }
