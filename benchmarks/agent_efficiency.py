#!/usr/bin/env python3
"""Quality-aware end-to-end efficiency comparison for µAgent.

The default smoke run is hermetic. Live provider runs are opt-in because they
are billable: pass --run and one or more --model values. Every model runs the
same read-only task once with compaction disabled and once with forced
mid-turn compaction, making quality regressions visible beside token, request,
latency, schema, cache, cost, binary-size, and peak-RSS metrics.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

PROMPT = """Analyze the cache-hit bug in this fixture without changing files.
Your first tool call must be read_path on ISSUE.md. Then inspect src/cache.py
and tests/test_cache.py with read_path, using one parallel batch where possible.
Return four clearly labeled Markdown items covering defect, fix, regression
test, and scope; bullets or bold headings are both acceptable. The answer must
name src/cache.py, `key in cache`, `cache[key]`, and test_cache_hit. Do not use
run, edit_file, delegation, memory, or web tools."""

FINAL_ANSWER = """- Defect: `src/cache.py` returns `None` when `key in cache`.
- Fix: return `cache[key]` from the cache-hit branch.
- Regression: `tests/test_cache.py::test_cache_hit` must assert the cached value.
- Scope: cache misses still compute, store, and return the loader result."""


def sse(payload: dict[str, Any]) -> bytes:
    return (f"data: {json.dumps(payload)}\n\ndata: [DONE]\n\n").encode()


def event(content: str, *, usage: dict[str, Any] | None = None) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "choices": [{"delta": {"content": content}, "finish_reason": "stop"}]
    }
    if usage is not None:
        payload["usage"] = usage
    return payload


def tool_calls(calls: list[tuple[str, str, dict[str, Any]]]) -> dict[str, Any]:
    return {
        "choices": [
            {
                "delta": {
                    "tool_calls": [
                        {
                            "index": index,
                            "id": call_id,
                            "function": {
                                "name": name,
                                "arguments": json.dumps(arguments),
                            },
                        }
                        for index, (call_id, name, arguments) in enumerate(calls)
                    ]
                },
                "finish_reason": "tool_calls",
            }
        ],
        "usage": {"prompt_tokens": 100, "completion_tokens": 8, "cost": 0.0001},
    }


class MockServer:
    def __init__(self) -> None:
        self.requests: list[dict[str, Any]] = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                size = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(size))
                owner.requests.append({"bytes": size, "body": body})
                messages = body.get("messages", [])
                last = str(messages[-1].get("content", "")) if messages else ""
                serialized = json.dumps(messages)
                results = [message for message in messages if message.get("role") == "tool"]
                if last.startswith("Summarize the bounded transcript") and not body.get("tools"):
                    response = event(
                        "The user requested a read-only diagnosis of ISSUE.md, "
                        "src/cache.py, and tests/test_cache.py. ISSUE.md has been read; "
                        "inspect both code files next and return exactly four bullets."
                    )
                elif not results and "ISSUE.md has been read" in serialized:
                    response = tool_calls(
                        [
                            ("source", "read_path", {"path": "src/cache.py"}),
                            ("test", "read_path", {"path": "tests/test_cache.py"}),
                        ]
                    )
                elif not results:
                    response = tool_calls([("issue", "read_path", {"path": "ISSUE.md"})])
                elif len(results) == 1:
                    response = tool_calls(
                        [
                            ("source", "read_path", {"path": "src/cache.py"}),
                            ("test", "read_path", {"path": "tests/test_cache.py"}),
                        ]
                    )
                else:
                    response = event(
                        FINAL_ANSWER,
                        usage={
                            "prompt_tokens": 120,
                            "completion_tokens": 55,
                            "prompt_tokens_details": {"cached_tokens": 80},
                            "cost": 0.0002,
                        },
                    )
                data = sse(response)
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *_: Any) -> None:
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.httpd.server_port}/v1"

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=2)


def write_fixture(workspace: Path) -> None:
    (workspace / "src").mkdir(parents=True)
    (workspace / "tests").mkdir()
    observations = "".join(
        f"- Observation {index:02d}: cache misses load and persist correctly; "
        "a subsequent hit returns no value.\n"
        for index in range(48)
    )
    (workspace / "ISSUE.md").write_text(
        "# Cache hit returns no value\n\n"
        "A populated cache unexpectedly returns None. The diagnostic history "
        "below deliberately makes this fixture large enough for a meaningful "
        "forced-compaction comparison.\n\n" + observations,
        encoding="utf-8",
    )
    (workspace / "src" / "cache.py").write_text(
        """def get_or_load(cache, key, loader):
    if key in cache:
        return None
    value = loader(key)
    cache[key] = value
    return value
""",
        encoding="utf-8",
    )
    (workspace / "tests" / "test_cache.py").write_text(
        """from src.cache import get_or_load


def test_cache_hit():
    assert get_or_load({"ready": 42}, "ready", lambda _: 0) == 42
""",
        encoding="utf-8",
    )


def snapshot(root: Path) -> dict[str, str]:
    return {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file() and ".uagent" not in path.parts
    }


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


def peak_rss(stderr: str) -> int:
    macos = re.search(r"(\d+)\s+maximum resident set size", stderr)
    if macos:
        return int(macos.group(1))
    linux = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", stderr)
    return int(linux.group(1)) * 1024 if linux else 0


def measured_command(binary: Path, arguments: list[str]) -> list[str]:
    timer = Path("/usr/bin/time")
    if not timer.is_file():
        return [str(binary), *arguments]
    if sys.platform == "darwin":
        return [str(timer), "-l", str(binary), *arguments]
    if sys.platform.startswith("linux"):
        return [str(timer), "-v", str(binary), *arguments]
    return [str(binary), *arguments]


def request_metrics(records: list[dict[str, Any]]) -> dict[str, int]:
    requests = [
        record.get("data", {}) for record in records if record.get("event") == "model_request"
    ]
    current_messages = 0
    estimated = []
    for request in requests:
        if request.get("projected_context"):
            message_chars = int(request.get("message_chars") or 0)
        elif "message_chars" in request:
            current_messages = int(request.get("message_chars") or 0)
            message_chars = current_messages
        else:
            current_messages += int(request.get("new_message_chars") or 0)
            message_chars = current_messages
        schema_chars = int(request.get("schema_chars") or 0) if request.get("native_tools") else 0
        estimated.append(message_chars + schema_chars)
    first = requests[0] if requests else {}
    return {
        "model_requests": len(requests),
        "estimated_request_chars": sum(estimated),
        "max_estimated_request_chars": max(estimated, default=0),
        "initial_schema_chars": int(first.get("schema_chars") or 0),
        "initial_tool_schemas": int(first.get("tool_schemas") or 0),
    }


def copy_user_config(home: Path) -> None:
    source = Path.home() / ".uagent" / ".config"
    if source.is_file():
        target = home / ".uagent" / ".config"
        target.parent.mkdir(parents=True)
        shutil.copyfile(source, target)
        target.chmod(0o600)


def quality(
    answer: str, calls: list[dict[str, Any]], unchanged: bool, returncode: int
) -> dict[str, Any]:
    read_paths = {
        str(call.get("arguments", {}).get("path", ""))
        for call in calls
        if call.get("name") == "read_path" and isinstance(call.get("arguments"), dict)
    }
    answer_lines = answer.splitlines()
    bullet_lines = [index for index, line in enumerate(answer_lines) if line.startswith("- ")]
    preamble = answer_lines[: bullet_lines[0]] if bullet_lines else answer_lines
    heading_shape = all(
        re.search(rf"(?im)^\s*(?:#{{1,6}}\s+{label}\s*$|\*\*{label}\*\*)", answer)
        for label in ("defect", "fix", "regression test", "scope")
    )
    checks = {
        "process_ok": returncode == 0,
        "workspace_unchanged": unchanged,
        "read_required_files": {"ISSUE.md", "src/cache.py", "tests/test_cache.py"} <= read_paths,
        # Wrapped bullet bodies are continuation lines. Providers also vary
        # between bullets and bold headings, so accept either four-item form.
        "answer_shape": heading_shape
        or (len(bullet_lines) == 4 and sum(len(line.strip()) for line in preamble) <= 80),
        "answer_evidence": all(
            marker in answer
            for marker in ("src/cache.py", "key in cache", "cache[key]", "test_cache_hit")
        ),
    }
    return {"passed": all(checks.values()), "score": sum(checks.values()), "checks": checks}


def run_case(
    binary: Path,
    model: str,
    variant: str,
    arguments: argparse.Namespace,
    mock: MockServer | None,
) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix=f"uagent-efficiency-{variant}-") as temporary:
        root = Path(temporary)
        workspace = root / "workspace"
        home = root / "home"
        workspace.mkdir()
        home.mkdir()
        write_fixture(workspace)
        if mock is None:
            copy_user_config(home)
        before = snapshot(workspace)
        trace_path = root / "trace.jsonl"

        env = dict(os.environ)
        env["HOME"] = str(home)
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
        if arguments.toolset != "full":
            env["UAGENT_TOOLSET"] = arguments.toolset
        else:
            env.pop("UAGENT_TOOLSET", None)
        if mock is not None:
            # Keep the hermetic loopback transport independent of runner-level
            # proxy configuration (notably macOS system libcurl behavior).
            for name in (
                "ALL_PROXY",
                "HTTP_PROXY",
                "HTTPS_PROXY",
                "all_proxy",
                "http_proxy",
                "https_proxy",
            ):
                env.pop(name, None)
            env["NO_PROXY"] = "127.0.0.1,localhost"
            env["no_proxy"] = env["NO_PROXY"]
            env.update(
                {
                    "UAGENT_BASE_URL": mock.url,
                    "UAGENT_API_KEY": "benchmark-placeholder",
                    "UAGENT_CONTEXT": "16384",
                    "UAGENT_REQUEST_TIMEOUT": "10",
                    "UAGENT_FIRST_EVENT_TIMEOUT": "5",
                    "UAGENT_STREAM_IDLE_TIMEOUT": "5",
                }
            )
            env.pop("UAGENT_PROVIDERS", None)

        cli = [
            "--json",
            "--no-memory",
            f"--debug={trace_path}",
            "--model",
            model,
            "--budget",
            str(arguments.max_cost),
            "-p",
            PROMPT,
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
        )
        elapsed = time.monotonic() - started
        try:
            envelope = json.loads(process.stdout)
        except json.JSONDecodeError:
            envelope = {}
        answer = str(envelope.get("answer") or "")
        records = read_trace(trace_path)
        calls = []
        for record in records:
            if record.get("event") != "tool_call":
                continue
            data = record.get("data", {})
            raw_arguments = data.get("arguments", {})
            if isinstance(raw_arguments, str):
                try:
                    raw_arguments = json.loads(raw_arguments)
                except json.JSONDecodeError:
                    raw_arguments = {}
            calls.append({"name": data.get("name"), "arguments": raw_arguments})
        compaction_outcomes = [
            record.get("data", {}).get("outcome")
            for record in records
            if record.get("event") == "compact_end"
        ]
        compactions = compaction_outcomes.count("ok")
        result = {
            "model": model,
            "variant": variant,
            "elapsed_seconds": round(elapsed, 3),
            "peak_rss_bytes": peak_rss(process.stderr),
            "usage": envelope.get("usage", {}),
            "routes": envelope.get("routes", {}),
            "tool_calls": len(calls),
            "calls": calls,
            "compactions": compactions,
            "compaction_outcomes": compaction_outcomes,
            "quality": quality(
                answer,
                calls,
                snapshot(workspace) == before,
                process.returncode,
            ),
            "answer": answer,
            "error": envelope.get("error")
            or (process.stderr.strip() if process.returncode else None),
            **request_metrics(records),
        }
        expected_compactions = compactions > 0 if variant == "compacted" else compactions == 0
        result["quality"]["checks"]["compaction_mode"] = expected_compactions
        result["quality"]["score"] += int(expected_compactions)
        result["quality"]["passed"] = result["quality"]["passed"] and expected_compactions
        return result


def print_results(results: list[dict[str, Any]], comparisons: list[dict[str, Any]]) -> None:
    print(
        "model                         mode       quality requests tools input cached output cost       wall   RSS"
    )
    for result in results:
        usage = result.get("usage", {})
        cost = f"${float(usage.get('cost') or 0):.4f}" if usage.get("cost_reported") else "n/a"
        rss = float(result["peak_rss_bytes"]) / (1024 * 1024)
        print(
            f"{result['model'][:29]:<29} {result['variant']:<10} "
            f"{result['quality']['score']}/6     {result['model_requests']:>3}      "
            f"{result['tool_calls']:>3} {int(usage.get('input') or 0):>5} "
            f"{int(usage.get('cache_read') or 0):>6} {int(usage.get('output') or 0):>6} "
            f"{cost:<9} {result['elapsed_seconds']:>5.1f}s {rss:>5.1f}M"
        )
    for comparison in comparisons:
        status = "PASS" if comparison["passed"] else "REGRESSION"
        print(
            f"{status}: {comparison['model']} compacted quality delta "
            f"{comparison['quality_delta']:+d}, request delta {comparison['request_delta']:+d}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="uagent binary to evaluate")
    parser.add_argument("--run", action="store_true", help="allow opt-in live provider calls")
    parser.add_argument("--model", action="append", default=[], help="model route; repeatable")
    parser.add_argument("--report", type=Path, help="write the full JSON report")
    parser.add_argument("--max-cost", type=float, default=0.10, help="reported USD cap per run")
    parser.add_argument("--timeout", type=int, default=300, help="seconds per run")
    parser.add_argument("--toolset", choices=("full", "lean"), default="full")
    arguments = parser.parse_args()
    arguments.binary = arguments.binary.resolve()
    if not arguments.binary.is_file():
        parser.error(f"binary does not exist: {arguments.binary}")
    if arguments.max_cost <= 0:
        parser.error("--max-cost must be positive")
    if arguments.run and not arguments.model:
        parser.error("--run requires at least one --model")
    if not arguments.run and arguments.model:
        parser.error("--model makes provider calls and therefore requires --run")
    return arguments


def main() -> int:
    arguments = parse_args()
    models = arguments.model if arguments.run else ["mock"]
    mock = None if arguments.run else MockServer()
    results = []
    try:
        for model in models:
            for variant in ("control", "compacted"):
                results.append(run_case(arguments.binary, model, variant, arguments, mock))
    finally:
        if mock is not None:
            mock.close()

    comparisons = []
    for model in models:
        control, compacted = [result for result in results if result["model"] == model]
        comparison = {
            "model": model,
            "quality_delta": compacted["quality"]["score"] - control["quality"]["score"],
            "request_delta": compacted["model_requests"] - control["model_requests"],
            "passed": (
                control["quality"]["passed"]
                and compacted["quality"]["passed"]
                and compacted["quality"]["score"] >= control["quality"]["score"]
            ),
        }
        comparisons.append(comparison)

    report = {
        "schema": "uagent.efficiency.v1",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "live": arguments.run,
        "binary": {
            "path": str(arguments.binary),
            "bytes": arguments.binary.stat().st_size,
            "sha256": hashlib.sha256(arguments.binary.read_bytes()).hexdigest(),
        },
        "toolset": arguments.toolset,
        "results": results,
        "comparisons": comparisons,
    }
    if mock is not None:
        report["mock_request_bytes"] = [request["bytes"] for request in mock.requests]
    if arguments.report:
        arguments.report.parent.mkdir(parents=True, exist_ok=True)
        arguments.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print_results(results, comparisons)
    return 0 if all(comparison["passed"] for comparison in comparisons) else 1


if __name__ == "__main__":
    raise SystemExit(main())
