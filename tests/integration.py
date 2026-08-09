#!/usr/bin/env python3
import base64
import errno
import fcntl
import json
import os
import pathlib
import pty
import re
import select
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from memory_fixture import global_memory_dir, project_memory_dir

# Enough of a PNG for the attachment inspector to accept it.
SMALL_PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 32
BINARY = pathlib.Path(sys.argv[1]).resolve()


def integration_group(name):
    """Keep integration domains isolated without duplicating shared fixtures."""
    groups = (
        ("mcp", ("mcp", "chrome_session")),
        ("checkpoint", ("checkpoint", "midturn_compaction")),
        ("delegation", ("subagent", "delegated_session", "parallel_subagents")),
        (
            "providers",
            (
                "model_",
                "provider_",
                "handoff_",
                "search_",
                "openrouter_",
                "streamed_search",
                "local_model_",
            ),
        ),
        (
            "ui",
            (
                "command_help",
                "multiline_",
                "signal_exit",
                "response_stats",
                "context_command",
                "memory_command",
                "input_redraw",
                "reconnect_",
                "narrow_terminal",
            ),
        ),
        (
            "tools",
            (
                "attach_",
                "tool_trace",
                "python_",
                "large_run_",
                "invalid_tool_",
                "terminal_image",
                "run_rejects",
                "grep_",
                "edit_",
                "directory_",
                "parallel_source",
                "external_read",
                "skill_tool",
                "parallel_run",
                "parallel_tool",
                "detached_terminal",
            ),
        ),
    )
    short_name = name.removeprefix("test_")
    for group, markers in groups:
        if any(marker in short_name for marker in markers):
            return group
    return "runtime"


def event(delta=None, finish="stop", usage=None):
    choice = {"delta": delta or {}, "finish_reason": finish}
    payload = {"choices": [choice]}
    if usage is not None:
        payload["usage"] = usage
    return payload


def sse(payload):
    return ("data: " + json.dumps(payload) + "\n\ndata: [DONE]\n\n").encode()


class Server:
    def __init__(self, responders, get_response=None):
        self.responders = list(responders)
        self.requests = []
        self.get_requests = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                owner.get_requests.append(self.path)
                if get_response is None:
                    self.send_error(404)
                    return
                data = json.dumps(get_response).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_POST(self):
                size = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(size))
                owner.requests.append((dict(self.headers), body))
                index = len(owner.requests) - 1
                response = owner.responders[min(index, len(owner.responders) - 1)]
                if callable(response):
                    response = response(self, body)
                    if response is None:
                        return
                streaming = body.get("stream", True)
                data = sse(response) if streaming else json.dumps(response).encode()
                self.send_response(200)
                self.send_header(
                    "Content-Type",
                    "text/event-stream" if streaming else "application/json",
                )
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                try:
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_):
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(
            target=self.httpd.serve_forever,
            kwargs={"poll_interval": 0.01},
            daemon=True,
        )
        self.thread.start()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.httpd.server_port}/v1"

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=2)


def base_env(home, url):
    env = {key: value for key, value in os.environ.items() if not key.startswith("UAGENT_")}
    env.update(
        {
            "HOME": str(home),
            "UAGENT_BASE_URL": url,
            "UAGENT_MODEL": "test",
            "UAGENT_CONTEXT": "4096",
            "UAGENT_REQUEST_TIMEOUT": "5",
            "UAGENT_FIRST_EVENT_TIMEOUT": "2",
            "UAGENT_STREAM_IDLE_TIMEOUT": "2",
            "UAGENT_CHROME_DEVTOOLS": "0",
        }
    )
    return env


def run(cwd, env, *args, timeout=10):
    return subprocess.run(
        [str(BINARY), *args],
        cwd=cwd,
        env=env,
        stdin=subprocess.DEVNULL,
        text=True,
        capture_output=True,
        timeout=timeout,
    )


def run_dialog(cwd, env, text, *args, timeout=10):
    return subprocess.run(
        [str(BINARY), *args],
        cwd=cwd,
        env=env,
        input=text,
        text=True,
        capture_output=True,
        timeout=timeout,
    )


def run_pty(cwd, env, payload=b"", interrupt=False, timeout=10, columns=80, args=()):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, columns, 0, 0))
    process = subprocess.Popen(
        [str(BINARY), *args],
        cwd=cwd,
        env=env,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        start_new_session=True,
    )
    os.close(slave)
    output = bytearray()
    deadline = time.monotonic() + timeout

    def read_until(marker=None, start=0, following=None):
        while time.monotonic() < deadline:
            if marker is not None:
                marker_at = output.find(marker, start)
                if marker_at >= 0:
                    after_marker = marker_at + len(marker)
                    if following is None or following in output[after_marker:]:
                        return True
            if select.select([master], [], [], 0.1)[0]:
                try:
                    output.extend(os.read(master, 65536))
                except OSError:
                    return False
            elif process.poll() is not None:
                return False
        return marker is None

    def read_prompt(start=0):
        read_until(b"\x1b[36m> \x1b[0m\x1b[39m\x1b[49m", start)
        time.sleep(0.05)  # the composer finishes raw-mode setup after drawing

    read_prompt()
    if interrupt:
        process.send_signal(signal.SIGINT)
    else:
        payloads = [payload] if isinstance(payload, bytes) else payload
        for index, item in enumerate(payloads):
            marker = None
            resized_columns = None
            if isinstance(item, tuple):
                if len(item) == 3:
                    item, marker, resized_columns = item
                else:
                    item, marker = item
                    resized_columns = None
            start = len(output)
            try:
                os.write(master, item)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                read_until()
                break
            if resized_columns:
                fcntl.ioctl(
                    master,
                    termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, resized_columns, 0, 0),
                )
                # This PTY is not the child's controlling terminal, so mirror
                # the SIGWINCH a real terminal sends to its foreground group.
                process.send_signal(signal.SIGWINCH)
                time.sleep(0.05)
            if index + 1 < len(payloads):
                if marker is not None and not read_until(marker, start):
                    break
                if marker is None:
                    read_prompt(start)
    read_until()
    if process.poll() is None:
        process.kill()
    process.wait()
    os.close(master)
    return process.returncode, bytes(output)


def assert_true(value, message):
    if not value:
        raise AssertionError(message)


def function_tools(body):
    return [tool["function"] for tool in body.get("tools", []) if tool.get("type") == "function"]


def function_names(body):
    return {tool["name"] for tool in function_tools(body)}


def function_tool(body, name):
    return next(tool for tool in function_tools(body) if tool["name"] == name)


def large_json_command():
    payload = {
        "sentinel": "HEAD-ONLY",
        "padding": "x" * 12000,
        "tail": "FULL-END",
    }
    script = (
        "import json;"
        "print(json.dumps({'sentinel':'HEAD-ONLY','padding':'x'*12000,"
        "'tail':'FULL-END'},separators=(',',':')))"
    )
    command = f"{shlex.quote(sys.executable)} -c {shlex.quote(script)}"
    return command, len((json.dumps(payload, separators=(",", ":")) + "\n").encode())


def captured_log_path(result):
    prefix = "[captured log: "
    start = result.find(prefix)
    end = result.find(" (", start + len(prefix))
    assert_true(start >= 0 and end > start, result)
    return pathlib.Path(result[start + len(prefix) : end])


def json_sentinel_command(path):
    query = "import json,sys;print(json.load(open(sys.argv[1], encoding='utf-8'))['sentinel'])"
    return f"{shlex.quote(sys.executable)} -c {shlex.quote(query)} {shlex.quote(str(path))}"


def test_plain_turn(root, home):
    def reply(_, body):
        names = function_names(body)
        assert_true("activity_output" not in names, names)
        return event({"content": "ok"}, usage={"prompt_tokens": 2, "completion_tokens": 1})

    server = Server([reply])
    try:
        result = run(root, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "ok", result.stdout)
    finally:
        server.close()


def test_stream_error_is_not_an_empty_response(root, home):
    server = Server([{"error": {"message": "upstream overloaded", "type": "server_error"}}])
    try:
        result = run(root, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode != 0, result.stdout)
        assert_true("upstream overloaded" in result.stderr, result.stderr)
        assert_true("empty response" not in result.stderr, result.stderr)
        assert_true(len(server.requests) == 3, server.requests)
    finally:
        server.close()


def test_empty_response_after_tools_recovers_once(root, home):
    def recovered(_, body):
        contents = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "system"
        ]
        valid = any(
            "Return the final answer from existing results" in str(content) for content in contents
        )
        return event({"content": "recovered" if valid else "missing-recovery"})

    server = Server(
        [
            tool_call("list_dir", {"path": "."}),
            event(),
            recovered,
        ]
    )
    try:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "inspect")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "recovered", result.stdout)
        assert_true(len(server.requests) == 3, len(server.requests))
    finally:
        server.close()

    exhausted = Server(
        [
            tool_call("list_dir", {"path": "."}),
            event(),
            event(),
        ]
    )
    try:
        result = run(root, base_env(home, exhausted.url), "--yolo", "-p", "inspect")
        assert_true(result.returncode != 0, result.stdout)
        assert_true("model returned an empty response" in result.stderr, result.stderr)
        assert_true(len(exhausted.requests) == 3, len(exhausted.requests))
    finally:
        exhausted.close()


def test_failed_empty_recovery_is_removed(root, home):
    def verify(_, body):
        stale = any(
            "Return the final answer from existing results" in str(message.get("content", ""))
            for message in body["messages"]
        )
        return event({"content": "stale-recovery" if stale else "recovery-clean"})

    server = Server(
        [
            tool_call("list_dir", {"path": "."}),
            event(),
            event(),
            verify,
        ]
    )
    try:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "inspect\nnext question\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("recovery-clean" in result.stdout, result.stdout)
        assert_true("stale-recovery" not in result.stdout, result.stdout)
    finally:
        server.close()


def test_transient_stream_errors_retry_before_progress(root, home):
    server = Server(
        [
            {
                "error": {
                    "message": (
                        "Codex response failed: {'type': 'service_unavailable_error', "
                        "'code': 'server_is_overloaded', 'message': 'overloaded'}"
                    ),
                    "type": "proxy_error",
                }
            },
            {
                "error": {
                    "message": "server overloaded",
                    "type": "service_unavailable_error",
                    "code": "server_is_overloaded",
                }
            },
            event({"content": "retry-ok"}),
        ]
    )
    try:
        result = run(root, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "retry-ok", result.stdout)
        assert_true(len(server.requests) == 3, server.requests)
    finally:
        server.close()


def test_transient_http_error_retries_before_progress(root, home):
    def unavailable(handler, _):
        data = json.dumps(
            {
                "error": {
                    "message": "temporarily unavailable",
                    "type": "service_unavailable_error",
                }
            }
        ).encode()
        handler.send_response(503)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(data)))
        handler.end_headers()
        handler.wfile.write(data)

    server = Server([unavailable, event({"content": "http-retry-ok"})])
    try:
        result = run(root, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "http-retry-ok", result.stdout)
        assert_true(len(server.requests) == 2, server.requests)
    finally:
        server.close()


def test_connection_drop_retries_before_progress(root, home):
    def disconnect(handler, _):
        handler.connection.shutdown(socket.SHUT_RDWR)
        handler.connection.close()

    server = Server([disconnect, event({"content": "connection-retry-ok"})])
    try:
        result = run(root, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "connection-retry-ok", result.stdout)
        assert_true(len(server.requests) == 2, server.requests)
    finally:
        server.close()


def test_connection_timeout_retries_inside_turn_budget(root, home):
    def stall(_, __):
        time.sleep(2)

    server = Server([stall, event({"content": "timeout-retry-ok"})])
    try:
        env = base_env(home, server.url)
        env["UAGENT_REQUEST_TIMEOUT"] = "1"
        env["UAGENT_FIRST_EVENT_TIMEOUT"] = "0"
        env["UAGENT_STREAM_IDLE_TIMEOUT"] = "0"
        env["UAGENT_MAX_TURN_SECONDS"] = "6"
        result = run(root, env, "-p", "reply", timeout=8)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "timeout-retry-ok", result.stdout)
        assert_true(len(server.requests) == 2, server.requests)
    finally:
        server.close()


def test_reasoning_only_timeout_is_safe_to_retry(root, home):
    def reason_then_stall(handler, _):
        payload = json.dumps(
            {"choices": [{"delta": {"reasoning": "partial"}, "finish_reason": None}]}
        ).encode()
        handler.send_response(200)
        handler.send_header("Content-Type", "text/event-stream")
        handler.end_headers()
        handler.wfile.write(b"data: " + payload + b"\n\n")
        handler.wfile.flush()
        time.sleep(2)

    server = Server([reason_then_stall, event({"content": "reason-retry-ok"})])
    try:
        env = base_env(home, server.url)
        env["UAGENT_REQUEST_TIMEOUT"] = "1"
        env["UAGENT_FIRST_EVENT_TIMEOUT"] = "0"
        env["UAGENT_STREAM_IDLE_TIMEOUT"] = "0"
        env["UAGENT_MAX_TURN_SECONDS"] = "6"
        result = run(root, env, "-p", "reply", timeout=8)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "reason-retry-ok", result.stdout)
        assert_true(len(server.requests) == 2, server.requests)
    finally:
        server.close()


def test_openrouter_typed_timeout_retries(root, home):
    server = Server(
        [
            {
                "error": {
                    "code": 504,
                    "message": "provider timed out",
                    "metadata": {"error_type": "timeout"},
                }
            },
            event({"content": "typed-timeout-retry-ok"}),
        ]
    )
    try:
        result = run(root, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "typed-timeout-retry-ok", result.stdout)
        assert_true(len(server.requests) == 2, server.requests)
    finally:
        server.close()


def test_empty_response_does_not_enter_history(root, home):
    def verify(_, body):
        empty_assistant = any(
            message.get("role") == "assistant" and not message.get("content")
            for message in body["messages"]
        )
        return event({"content": "history-bad" if empty_assistant else "history-clean"})

    server = Server([event(), verify])
    try:
        result = run_dialog(root, base_env(home, server.url), "first\nsecond\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true("history-clean" in result.stdout, result.stdout)
        assert_true("history-bad" not in result.stdout, result.stdout)
    finally:
        server.close()


def test_command_help(root, home):
    server = Server([event({"content": "unused"})])
    try:
        result = run_dialog(root, base_env(home, server.url), "/models\n/wat\n/recap\n/help\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true("unknown command /wat; use /help" in result.stdout, result.stdout)
        assert_true("unknown command /recap; use /help" in result.stdout, result.stdout)
        assert_true("commands\n" in result.stdout, result.stdout)
        assert_true("  /attach PATH" in result.stdout, result.stdout)
        assert_true("attach a file to the next turn" in result.stdout, result.stdout)
        assert_true("  /help" in result.stdout, result.stdout)
        assert_true("show this help" in result.stdout, result.stdout)
        assert_true("commands:" not in result.stdout, result.stdout)
        assert_true("use /models QUERY" in result.stdout, result.stdout)
        assert_true(not server.get_requests, server.get_requests)
    finally:
        server.close()


def test_project_instructions_precede_first_turn(root, home):
    workspace = root / "instructions-workspace"
    nested = workspace / "nested"
    (workspace / ".git").mkdir(parents=True)
    nested.mkdir()
    (workspace / "AGENTS.md").write_text("root-agent-sentinel", encoding="utf-8")
    # CLAUDE.md is a fallback: shadowed here by AGENTS.md in the same directory
    (workspace / "CLAUDE.md").write_text("shadowed-claude-sentinel", encoding="utf-8")
    (nested / "AGENTS.override.md").write_text("nested-agent-sentinel", encoding="utf-8")

    def verify(_, body):
        messages = body["messages"]
        instructions = messages[1].get("content", "") if len(messages) > 1 else ""
        contents = [str(m.get("content", "")) for m in messages]
        valid = (
            messages[0].get("role") == "system"
            and messages[1].get("role") == "system"
            and any(
                message.get("role") == "system"
                and str(message.get("content", "")).startswith("[environment:")
                for message in messages
            )
            and [message.get("content") for message in messages if message.get("role") == "user"]
            == ["reply"]
            and "reply" in contents
            and contents.index("reply") > 1  # instructions precede the prompt
            and "<INSTRUCTIONS>" in instructions
            and "shadowed-claude-sentinel" not in instructions
            and instructions.index("root-agent-sentinel")
            < instructions.index("nested-agent-sentinel")
        )
        return event({"content": "instructions-ok" if valid else "instructions-bad"})

    server = Server([verify])
    try:
        result = run(nested, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "instructions-ok", result.stdout)
    finally:
        server.close()


def test_attach_tool_puts_bytes_in_context(root, home):
    """The model can pull an image and a document into its own context, and the
    encoded bytes do not stay in history afterwards."""
    workspace = root / "attach-workspace"
    workspace.mkdir()
    png = workspace / "shot.png"
    png.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\x00" * 32)
    pdf = workspace / "paper.pdf"
    # Large enough to exceed the synthetic 4K context estimate if encoded
    # bytes are incorrectly offered to mid-turn compaction.
    pdf.write_bytes(b"%PDF-1.4\n" + b"\x00" * (1024 * 1024))
    seen = {}

    def route(_, body):
        messages = body["messages"]
        parts = [p for m in messages if isinstance(m.get("content"), list) for p in m["content"]]
        if parts:
            seen["image"] = any(p.get("type") == "image_url" for p in parts)
            seen["file"] = any(p.get("type") == "file" for p in parts)
            return event({"content": "attach-ok"})
        return event(  # both in one batch: attach is parallel_safe
            {
                "tool_calls": [
                    {
                        "index": i,
                        "id": f"call-{i}",
                        "function": {
                            "name": "attach",
                            "arguments": json.dumps({"path": str(path)}),
                        },
                    }
                    for i, path in enumerate((png, pdf))
                ]
            },
            finish="tool_calls",
        )

    server = Server([route])
    try:
        trace = workspace / "trace.jsonl"
        result = run(
            workspace,
            base_env(home, server.url),
            "--yolo",
            f"--debug={trace}",
            "-p",
            "look",
            timeout=40,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "attach-ok", result.stdout)
        assert_true(seen.get("image"), seen)
        assert_true(seen.get("file"), seen)
        assert_true(len(server.requests) == 2, len(server.requests))
        events = [json.loads(line) for line in trace.read_text().splitlines()]
        assert_true(not any(event["event"] == "midturn_compact" for event in events), events)
        # the base64 payload must not survive into the saved session
        sessions = list((home / ".uagent" / "history").rglob("*.json"))
        blobs = [
            s for s in sessions if "base64," in s.read_text(encoding="utf-8", errors="replace")
        ]
        assert_true(not blobs, blobs)
    finally:
        server.close()


def test_attach_flag_headless(root, home):
    """--attach sends a file with the first message, and rejects bad paths."""
    workspace = root / "attach-flag-workspace"
    workspace.mkdir()
    png = workspace / "flag.png"
    png.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\x00" * 16)
    seen = {}

    def verify(_, body):
        parts = [
            p for m in body["messages"] if isinstance(m.get("content"), list) for p in m["content"]
        ]
        seen["image"] = any(p.get("type") == "image_url" for p in parts)
        return event({"content": "flag-attach-ok"})

    server = Server([verify])
    try:
        env = base_env(home, server.url)
        result = run(workspace, env, "--attach", str(png), "-p", "look")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "flag-attach-ok", result.stdout)
        assert_true(seen.get("image"), seen)

        missing = run(workspace, env, "--attach", str(workspace / "nope.png"), "-p", "look")
        assert_true(missing.returncode == 2, missing.returncode)
    finally:
        server.close()


def test_full_run_and_python_terminal_trace(root, home):
    shell_command = "printf 'shell-one\\n'\nprintf 'shell-two\\n'"
    python_code = "print('python-one')\nprint('python-two')"
    server = Server(
        [
            tool_call("run", {"command": shell_command, "shell": "/bin/sh"}),
            tool_call(
                "run_python",
                {"path": "trace.py", "code": python_code, "packages": []},
            ),
            event({"content": "trace-ok"}),
        ]
    )
    try:
        env = base_env(home, server.url)
        env["UAGENT_TOOL_BATCH_RESULT_CHARS"] = "8"
        result = run_dialog(
            root,
            env,
            "/verbose\ntrace\n/q\n",
            "--yolo",
            timeout=20,
        )
        assert_true(result.returncode == 0, result.stderr)
        for expected in (
            "printf 'shell-one",
            "printf 'shell-two",
            "shell-one",
            "shell-two",
            "run_python(trace.py (create))",
            "[script: .uagent/scratch/trace.py]",
            "python-one",
            "python-two",
            "trace-ok",
        ):
            assert_true(expected in result.stdout, result.stdout)
    finally:
        server.close()


def test_tool_traces_persist_until_compaction(root, home):
    python_code = "from pathlib import Path\nPath('python.txt').write_text('PYTHON-SOURCE')"
    shell_command = "printf SHELL-SOURCE > shell.txt"

    def use_python_trace(_, body):
        calls = [call for message in body["messages"] for call in message.get("tool_calls", [])]
        retained = any(
            call.get("function", {}).get("name") == "run_python"
            and "PYTHON-SOURCE" in call.get("function", {}).get("arguments", "")
            for call in calls
        )
        return (
            tool_call("run", {"command": shell_command})
            if retained
            else event({"content": "python-trace-missing"})
        )

    def verify_all_traces(_, body):
        calls = [call for message in body["messages"] for call in message.get("tool_calls", [])]
        names = [call.get("function", {}).get("name") for call in calls]
        valid = names == ["run_python", "run"] and root.joinpath("python.txt").exists()
        valid = valid and root.joinpath("shell.txt").exists()
        return event({"content": "tool-retention-ok" if valid else "tool-retention-bad"})

    server = Server(
        [
            tool_call(
                "run_python",
                {"path": "retained.py", "code": python_code, "packages": []},
            ),
            event({"content": "python-created"}),
            use_python_trace,
            event({"content": "shell-created"}),
            verify_all_traces,
        ]
    )
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "131072"
        result = run_dialog(
            root,
            env,
            "create it\nmodify it\ncheck history\n/q\n",
            "--yolo",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("python-trace-missing" not in result.stdout, result.stdout)
        assert_true("tool-retention-ok" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 5, server.requests)
    finally:
        server.close()


def test_python_scratch_script_can_be_edited_and_rerun(root, home):
    script = root / ".uagent/scratch/calculation.py"

    def edit_saved_script(_, body):
        results = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        valid = script.is_file() and any(
            "[script: .uagent/scratch/calculation.py]" in value and "value=6" in value
            for value in results
        )
        return (
            tool_call(
                "edit_file",
                {
                    "path": str(script),
                    "edits": [{"old": "value = 2 * 3", "new": "value = 2 * 5"}],
                },
            )
            if valid
            else event({"content": "scratch-create-bad"})
        )

    def rerun_saved_script(_, body):
        return tool_call(
            "run_python",
            {"path": "calculation.py", "code": None, "packages": None},
        )

    def verify_rerun(_, body):
        results = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        valid = any("value=10" in value for value in results)
        source = script.read_text(encoding="utf-8") if script.exists() else ""
        valid = valid and "# /// script" in source and "value = 2 * 5" in source
        ignored = root / ".uagent/scratch/.gitignore"
        valid = valid and ignored.read_text(encoding="utf-8") == "*\n"
        return event({"content": "scratch-rerun-ok" if valid else "scratch-rerun-bad"})

    server = Server(
        [
            tool_call(
                "run_python",
                {
                    "path": "calculation.py",
                    "code": "value = 2 * 3\nprint(f'value={value}')",
                    "packages": [],
                },
            ),
            edit_saved_script,
            rerun_saved_script,
            verify_rerun,
        ]
    )
    try:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "calculate")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "scratch-rerun-ok", result.stdout)
        assert_true(len(server.requests) == 4, server.requests)
    finally:
        server.close()


def test_large_run_output_is_recoverable(root, home):
    command, expected_bytes = large_json_command()
    artifact = {}

    def inspect_result(_, body):
        result = next(
            message["content"]
            for message in reversed(body["messages"])
            if message.get("role") == "tool"
        )
        path = captured_log_path(result)
        artifact["path"] = path
        assert_true(len(result) <= 512, len(result))
        assert_true("FULL-END" in result, result)
        assert_true("HEAD-ONLY" not in result, result)
        assert_true(path.exists(), path)
        assert_true(path.stat().st_size == expected_bytes, path.stat().st_size)
        return tool_call("run", {"command": json_sentinel_command(path)})

    def verify_recovery(_, body):
        results = [
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        ]
        recovered = results[-1].strip() == "HEAD-ONLY"
        return event({"content": "artifact-ok" if recovered else "artifact-bad"})

    server = Server(
        [
            tool_call("run", {"command": command}),
            inspect_result,
            verify_recovery,
        ]
    )
    try:
        env = base_env(home, server.url)
        env["UAGENT_TOOL_RESULT_CHARS"] = "512"
        env["UAGENT_CONTEXT"] = "131072"
        result = run(root, env, "--yolo", "-p", "inspect large output")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "artifact-ok", result.stdout)
        assert_true(artifact["path"].exists(), artifact)
    finally:
        server.close()


def test_compact_tool_trace_is_default(root, home):
    command = "printf 'compact-visible\\n'\nprintf 'compact-hidden\\n'"
    server = Server(
        [
            tool_call("run", {"command": command, "shell": "/bin/sh"}),
            event({"content": "compact-trace-ok"}),
        ]
    )
    try:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "trace\n/q\n",
            "--yolo",
            timeout=20,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(command in result.stdout, result.stdout)
        assert_true("compact-visible" in result.stdout, result.stdout)
        assert_true("← run: compact-visible" in result.stdout, result.stdout)
        assert_true("← run: compact-hidden" not in result.stdout, result.stdout)
        assert_true("compact-trace-ok" in result.stdout, result.stdout)
    finally:
        server.close()


def test_edit_view_shows_compact_colored_diff(root, home):
    target = root / "edit-view.txt"
    target.write_text("before\nkeep\n", encoding="utf-8")
    server = Server(
        [
            tool_call(
                "edit_file",
                {
                    "path": str(target),
                    "edits": [{"old": "before", "new": "after"}],
                },
            ),
            event({"content": "edit-view-ok"}),
        ]
    )
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [b"edit it\n", b"/q\n"],
            args=("--yolo",),
            timeout=20,
        )
        assert_true(code == 0, output)
        assert_true(b"\xe2\x80\xa2" in output and b"Edited " in output, output)
        assert_true(b"-before" in output and b"+after" in output, output)
        assert_true(b"\x1b[31m" in output and b"\x1b[32m" in output, output)
        assert_true(b"edit-view-ok" in output, output)
        assert_true(target.read_text(encoding="utf-8") == "after\nkeep\n", output)
    finally:
        server.close()


def test_resumed_tool_trace_stays_compact(root, home):
    command = "printf 'resume-visible\\n'\nprintf 'resume-hidden\\n'"
    server = Server(
        [
            tool_call("run", {"command": command, "shell": "/bin/sh"}),
            event({"content": "resume-trace-ready"}),
        ]
    )
    try:
        env = base_env(home, server.url)
        code, first = run_pty(
            root,
            env,
            [b"create trace\n", b"/q\n"],
            args=("--yolo",),
            timeout=20,
        )
        assert_true(code == 0, first)
        assert_true(list((home / ".uagent" / "history").rglob("*.json")), first)

        code, resumed = run_pty(root, env, b"/q\n", args=("-c",), timeout=20)
        text = resumed.decode(errors="replace")
        assert_true(code == 0, text)
        assert_true("resume-visible" in text, text)
        assert_true("resume-hidden" not in text, text)
        assert_true("2 lines" in text, text)
        assert_true("resume-trace-ready" in text, text)
    finally:
        server.close()


def test_invalid_tool_trace_is_visible(root, home):
    server = Server(
        [
            tool_call("missing_tool", {}),
            event({"content": "invalid-trace-ok"}),
        ]
    )
    try:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "trace\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("→ missing_tool" in result.stdout, result.stdout)
        assert_true("← missing_tool: failed: error: unknown tool" in result.stdout, result.stdout)
        assert_true("invalid-trace-ok" in result.stdout, result.stdout)
    finally:
        server.close()


def test_terminal_image_capability_contract(root, home):
    with (
        tempfile.TemporaryDirectory() as empty_path,
        tempfile.TemporaryDirectory() as chafa_path,
    ):
        invoked = pathlib.Path(chafa_path) / "invoked"
        chafa = pathlib.Path(chafa_path) / "chafa"
        chafa.write_text(
            f"#!/bin/sh\ntouch {shlex.quote(str(invoked))}\n",
            encoding="utf-8",
        )
        chafa.chmod(0o755)

        def image_env(url, program, term, path):
            env = base_env(home, url)
            env.update({"TERM_PROGRAM": program, "TERM": term, "PATH": path})
            for variable in ("TMUX", "KITTY_WINDOW_ID", "LC_TERMINAL"):
                env.pop(variable, None)
            return env

        cases = (
            ("Apple_Terminal", "xterm-256color", False, empty_path),
            ("ghostty", "xterm-ghostty", True, empty_path),
            ("Apple_Terminal", "xterm-256color", False, chafa_path),
        )
        instruction = "Use show_image to display local images"
        for program, term, has_tool, path in cases:

            def verify(_, body, has_tool=has_tool):
                names = function_names(body)
                has_instruction = instruction in body["messages"][0]["content"]
                has_image_tool = "show_image" in names
                valid = has_instruction == has_tool and has_image_tool == has_tool
                return event(
                    {
                        "content": (
                            "image-capability-ok"
                            if valid
                            else "image-capability-bad "
                            f"instruction={has_instruction} "
                            f"show_image={has_image_tool}"
                        )
                    }
                )

            server = Server([verify])
            try:
                env = image_env(server.url, program, term, path)
                code, output = run_pty(root, env, [b"show an image\n", b"/q\n"])
                assert_true(code == 0, output)
                assert_true(b"image-capability-ok" in output, output)
            finally:
                server.close()
        assert_true(not invoked.exists(), "Chafa must not enable ASCII image output")

        image = pathlib.Path(chafa_path) / "tiny.png"
        image.write_bytes(b"\x89PNG\r\n\x1a\n")

        def final(_, body):
            result = next(
                message["content"] for message in body["messages"] if message.get("role") == "tool"
            )
            return event(
                {
                    "content": (
                        "native-image-ok" if "inline via kitty" in result else "native-image-bad"
                    )
                }
            )

        server = Server([tool_call("show_image", {"path": str(image)}), final])
        try:
            env = image_env(server.url, "ghostty", "xterm-ghostty", chafa_path)
            code, output = run_pty(chafa_path, env, [b"show an image\n", b"/q\n"], columns=40)
            assert_true(code == 0, output)
            assert_true(b"native-image-ok" in output, output)
            assert_true(b"\x1b_Ga=T,f=100,c=39" in output, output)
            assert_true(not invoked.exists(), "PNG rendering must not invoke Chafa")
        finally:
            server.close()


def test_multiline_bracketed_paste(root, home):
    def verify(_, body):
        pasted = body["messages"][-1].get("content")
        return event(
            {
                "content": "multiline-paste-ok"
                if pasted == "first line\nsecond line\nthird line"
                else "multiline-paste-bad"
            }
        )

    server = Server([verify])
    try:
        paste = b"\x1b[200~first line\nsecond line\nthird line\x1b[201~"
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(paste, b"second"), b"\n", b"\x04"],
            columns=24,
        )
        assert_true(code == 0, output)
        assert_true(b"multiline-paste-ok" in output, output)
        assert_true(b"ctx " in output, output)
        assert_true(b"\x1b[?2004h" in output and b"\x1b[?2004l" in output, output)
        assert_true(b"\x1b[48;5;" not in output, output)
        assert_true(b"\x1b[36m> \x1b[0m\x1b[39m\x1b[49m" in output, output)
        assert_true(len(server.requests) == 1, len(server.requests))
    finally:
        server.close()


def test_signal_exit_restores_terminal(root, home):
    server = Server([event({"content": "unused"})])
    try:
        code, output = run_pty(root, base_env(home, server.url), interrupt=True)
        restore = b"\x1b[0m\x1b[39m\x1b[49m"
        assert_true(code == 130, (code, output))
        assert_true(output.rfind(restore) > output.rfind(b"\x1b[48;5;"), output)
    finally:
        server.close()


def test_response_stats(root, home):
    def delayed_response(handler, __):
        time.sleep(0.1)
        first = ("data: " + json.dumps(event({"content": "stats-ok"})) + "\n\n").encode()
        final = (
            "data: "
            + json.dumps(
                event(
                    {},
                    usage={
                        "completion_tokens": 10,
                        "completion_tokens_details": {"reasoning_tokens": 6},
                    },
                )
            )
            + "\n\ndata: [DONE]\n\n"
        ).encode()
        handler.send_response(200)
        handler.send_header("Content-Type", "text/event-stream")
        handler.end_headers()
        handler.wfile.write(first)
        handler.wfile.flush()
        time.sleep(0.1)
        handler.wfile.write(final)
        handler.wfile.flush()

    server = Server([delayed_response])
    try:
        trace = root / "response-stats.jsonl"
        result = run_dialog(
            root,
            base_env(home, server.url),
            "hello\n/q\n",
            f"--debug={trace}",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("tok/s" in result.stdout, result.stdout)
        assert_true("first " in result.stdout, result.stdout)
        lines = result.stdout.splitlines()
        stats_index = next(index for index, line in enumerate(lines) if "tok/s" in line)
        throughput = float(lines[stats_index].split(" tok/s")[0].rsplit(" · ", 1)[1])
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        turn_end = next(record["data"] for record in records if record["event"] == "turn_end")
        expected = turn_end["generated_tokens"] * 1000 / turn_end["generation_ms"]
        assert_true(turn_end["generated_tokens"] == 10, turn_end)
        assert_true(abs(throughput - expected) < 0.06, (throughput, turn_end))
        assert_true("(+6 reasoning)" in lines[stats_index], result.stdout)
        assert_true("ctx " in lines[stats_index + 1], result.stdout)
    finally:
        server.close()


def test_response_stats_spinner_context(root, home):
    def delayed_response(_, __):
        time.sleep(0.25)
        return event(
            {"content": "spinner-ok"},
            usage={
                "completion_tokens": 4,
                "completion_tokens_details": {"reasoning_tokens": 3},
            },
        )

    server = Server([delayed_response])
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"hello\n", b"working \xc2\xb7"), b"/q\n"],
        )
        assert_true(code == 0, output)
        regular_metadata = b"\x1b[0m\x1b[39m\x1b[49m\x1b[2m"
        assert_true(b"\x1b[1m\xc2\xb5Agent" in output, output)
        assert_true(regular_metadata + b"test (default)" in output, output)
        assert_true(b"working \xc2\xb7 0.0s \xc2\xb7 ctx " in output, output)
        assert_true(b" \xc2\xb7 bg:0" in output, output)
        assert_true(b"\xc2\xb7 0 in" not in output, output)
        assert_true(b"\x1b[3m(+3 reasoning)\x1b[23m" in output, output)
        assert_true(
            any(f" · 0.{tick}s".encode() in output for tick in range(1, 6)),
            output,
        )
    finally:
        server.close()


def test_response_stats_spinner_elapsed_spans_entire_turn(root, home):
    def delayed_tool(_, __):
        time.sleep(0.3)
        return tool_call("list_dir", {"path": "."})

    def delayed_final(_, __):
        time.sleep(0.3)
        return event({"content": "turn-timer-ok"})

    server = Server([delayed_tool, delayed_final])
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"inspect\n", b"working \xc2\xb7"), b"/q\n"],
            args=("--yolo",),
        )
        assert_true(code == 0, output)
        frames = re.findall(rb"working[^\r\n]*? \xc2\xb7 ([0-9]+\.[0-9])s", output)
        assert_true(frames and max(map(float, frames)) >= 0.5, output)
    finally:
        server.close()


def test_context_command(root, home):
    server = Server([])
    try:
        result = run_dialog(root, base_env(home, server.url), "/context\n/ctx\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.count('"role": "system"') == 2, result.stdout)
        assert_true(result.stdout.count('"content":') >= 2, result.stdout)
        assert_true(result.stdout.count('"messages":') == 2, result.stdout)
        assert_true(result.stdout.count('"tools":') == 2, result.stdout)
        assert_true(result.stdout.count('"name": "read_file"') == 2, result.stdout)
        assert_true(result.stdout.count('"model": "test"') == 2, result.stdout)
        assert_true(not server.requests, server.requests)
    finally:
        server.close()


def test_tool_trace_prunes_old_outputs_in_batches(root, home):
    sources = []
    for turn, fill in enumerate("xyzw", start=1):
        source = root / f"large-source-{turn}.txt"
        source.write_text(f"TRACE-ORIGINAL-{turn}-" + fill * 4000, encoding="utf-8")
        sources.append(source)
    observed = {"compacted": False}

    def request_read(turn):
        def respond(_, body):
            if turn == 4:
                tool_results = [
                    str(message.get("content", ""))
                    for message in body["messages"]
                    if message.get("role") == "tool"
                ]
                observed["compacted"] = any(
                    result.startswith("[old tool output compacted:") for result in tool_results
                )
            return tool_call("read_file", {"path": str(sources[turn - 1])})

        return respond

    responders = []
    for turn in range(1, 5):
        responders.extend([request_read(turn), event({"content": f"turn-{turn}-ok"})])
    server = Server(responders)
    try:
        trace = root / "trace.jsonl"
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "65536"
        env["UAGENT_AUTO_COMPACT_PCT"] = "0"
        env["UAGENT_TOOL_TRACE_PROTECT_CHARS"] = "0"
        env["UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS"] = "1"
        result = run_dialog(
            root,
            env,
            "one\ntwo\nthree\nfour\n/q\n",
            f"--debug={trace}",
            timeout=20,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(observed["compacted"], server.requests)
        events = [json.loads(line) for line in trace.read_text().splitlines()]
        prune = [event for event in events if event["event"] == "tool_trace_pruned"]
        assert_true(prune and prune[0]["data"]["results"] == 1, prune)
        full_results = [
            event
            for event in events
            if event["event"] == "tool_result" and "TRACE-ORIGINAL-" in event["data"]["result"]
        ]
        assert_true(len(full_results) == 4, len(full_results))
    finally:
        server.close()


def test_tool_trace_deduplicates_recent_unchanged_read(root, home):
    source = root / "same-source.txt"
    source.write_text("SAME-READ-" + "r" * 3000, encoding="utf-8")

    def verify(_, body):
        results = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        duplicate = results[-1].startswith("[unchanged duplicate;")
        return event({"content": "dedupe-ok" if duplicate else "dedupe-bad"})

    server = Server(
        [
            tool_call("read_file", {"path": str(source)}),
            event({"content": "first-ok"}),
            tool_call("read_file", {"path": str(source)}),
            verify,
        ]
    )
    try:
        trace = root / "dedupe.jsonl"
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "65536"
        env["UAGENT_AUTO_COMPACT_PCT"] = "0"
        result = run_dialog(
            root,
            env,
            "first\nsecond\n/q\n",
            f"--debug={trace}",
            timeout=20,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("dedupe-ok" in result.stdout, result.stdout)
        events = [json.loads(line) for line in trace.read_text().splitlines()]
        deduped = [event for event in events if event["event"] == "tool_result_deduplicated"]
        assert_true(len(deduped) == 1, deduped)
        assert_true(deduped[0]["data"]["original_chars"] > 3000, deduped)
    finally:
        server.close()


def test_memory_command_is_local_and_inspectable(root, home):
    workspace = root / "memory-command-workspace"
    workspace.mkdir()
    global_dir = global_memory_dir(home)
    project_dir = project_memory_dir(home, workspace)
    global_dir.mkdir(parents=True)
    project_dir.mkdir(parents=True)
    (global_dir / "preferences.md").write_text("concise answers")
    (project_dir / "build.md").write_text("use cmake presets")
    old_global = home / ".uagent" / "memory" / "old-global.md"
    old_project = workspace / ".uagent" / "memory" / "old-project.md"
    old_project.parent.mkdir(parents=True)
    old_global.write_text("must stay invisible")
    old_project.write_text("must stay invisible")
    server = Server([])
    try:
        result = run_dialog(workspace, base_env(home, server.url), "/memory\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true("memory recall on · contribute on" in result.stdout, result.stdout)
        assert_true("global/preferences" in result.stdout, result.stdout)
        assert_true("project/build" in result.stdout, result.stdout)
        assert_true("old-global" not in result.stdout, result.stdout)
        assert_true("old-project" not in result.stdout, result.stdout)
        assert_true(not server.requests, server.requests)
    finally:
        server.close()
        (global_dir / "preferences.md").unlink(missing_ok=True)
        (project_dir / "build.md").unlink(missing_ok=True)
        old_global.unlink(missing_ok=True)
        old_project.unlink(missing_ok=True)


def test_input_redraw_survives_terminal_resize_and_delete(root, home):
    original = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    expected = original[:-10] + "XYZ"

    def answer(_, body):
        user = next(
            message.get("content")
            for message in reversed(body["messages"])
            if message.get("role") == "user"
        )
        return event({"content": "resize-ok" if user == expected else "resize-bad"})

    server = Server([answer])
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (original.encode(), b"", 20),
                b"\x7f" * 10 + b"XYZ\n",
                b"/q\n",
            ],
            columns=80,
        )
        text = output.decode(errors="replace")
        assert_true(code == 0, text)
        assert_true("resize-ok" in text, text)
        assert_true(len(server.requests) == 1, server.requests)
    finally:
        server.close()


def test_reconnect_resize_burst_does_not_duplicate_prompt(root, home):
    server = Server([event({"content": "reconnect-ok"})])
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"abc", b"abc", 79),
                (b"", b"", 78),
                (b"", b"", 77),
                (b"", b"", 76),
                b"\n",
                b"/q\n",
            ],
            columns=80,
        )
        assert_true(code == 0, output)
        plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", output)
        assert_true(plain.count(b"> abc\r\n") == 1, output)
        assert_true(plain.count(b"> /q\r\n") == 1, output)
        assert_true(len(server.requests) == 1, server.requests)
    finally:
        server.close()


def test_run_rejects_python_and_sudo_before_execution(root, home):
    def after_python(_, body):
        results = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        assert_true(any("use run_python" in value for value in results), results)
        return tool_call("run", {"command": "sudo true"})

    def after_sudo(_, body):
        results = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        assert_true(
            any("privileged commands are unavailable" in value for value in results),
            results,
        )
        return event({"content": "guarded"})

    server = Server(
        [
            tool_call("run", {"command": "python -c 'print(1)'"}),
            after_python,
            after_sudo,
        ]
    )
    try:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "work")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "guarded", result.stdout)
    finally:
        server.close()


def test_narrow_terminal_context_and_table_fallback(root, home):
    table = "| Name | Description |\n| --- | --- |\n| alpha | a deliberately wide description |\n"
    server = Server([event({"content": table})])
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"show table\n", b"Description:"), b"/q\n"],
            columns=24,
        )
        assert_true(code == 0, output)
        assert_true(b"ctx " in output, output)
        assert_true(b"Description:" in output, output)
        assert_true(b"deliberately wide" in output, output)
        messages = server.requests[0][1]["messages"]
        environments = [
            str(message.get("content", ""))
            for message in messages
            if str(message.get("content", "")).startswith("[environment:")
        ]
        assert_true(len(environments) == 1, environments)
        assert_true("terminal_columns=24" in environments[0], environments)
    finally:
        server.close()


def test_cacheable_prefix_stable_across_turns(root, home):
    """Stable environment state is appended once, preserving the provider's
    cached prefix without paying for duplicate cwd/date messages."""
    server = Server([event({"content": "one"}), event({"content": "two"})])
    try:
        result = run_dialog(root, base_env(home, server.url), "first\nsecond\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(len(server.requests) == 2, len(server.requests))
        first, second = (body["messages"] for _, body in server.requests)
        assert_true(first[0] == second[0], (first[0], second[0]))
        assert_true("[environment:" not in first[0]["content"], first[0]["content"])
        environments = [m for m in second if str(m.get("content", "")).startswith("[environment:")]
        assert_true(len(environments) == 1, environments)
        # every message the first request sent is still a byte-identical prefix
        assert_true(second[: len(first)] == first, (first, second[: len(first)]))
    finally:
        server.close()


def test_queue_then_escape_while_waiting_for_task(root, home):
    workspace = root / "steer-task-workspace"
    workspace.mkdir()

    def route(_, body):
        user_text = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "user"
        ]
        if "child" in user_text:
            time.sleep(30)
            return event({"content": "child-finished"})
        if "stop now" in user_text:
            return event({"content": "steering-ok"})
        results = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        if any("[started] task id" in result for result in results):
            return tool_call("activity_wait", {"wait_ms": 30000})
        return tool_call("task", {"prompt": "child"})

    server = Server([route])
    try:
        code, output = run_pty(
            workspace,
            base_env(home, server.url),
            [
                (b"delegate\n", b"activity_wait("),
                (b"stop now\n", b"steer:1"),
                (b"\x1b", b"test (default) @ 127.0.0.1"),
                b"/q\n",
            ],
            args=("--yolo",),
            timeout=12,
        )
        assert_true(code == 0, output)
        assert_true(b"steer:1" in output, output)
        assert_true(b"interrupting" in output, output)
        assert_true(b"steering-ok" in output, output)
        assert_true(b"child-finished" not in output, output)
    finally:
        server.close()


def test_idle_agent_resumes_on_activity_completion(root, home):
    workspace = root / "activity-resume-workspace"
    workspace.mkdir()

    def route(_, body):
        messages = body["messages"]
        if any(
            message.get("role") == "user" and message.get("content") == "child"
            for message in messages
        ):
            time.sleep(0.5)
            return event({"content": "child-finished"})
        combined = "\n".join(str(message.get("content", "")) for message in messages)
        if "[Background result: task id " in combined:
            return event({"content": "idle-resume-ok"})
        if "[started] task id " in combined:
            # End the parent turn while its child is still running. The TUI
            # activity event must initiate the next model call without input.
            return event({"content": "child-running"})
        return tool_call("task", {"prompt": "child"})

    server = Server([route])
    try:
        code, output = run_pty(
            workspace,
            base_env(home, server.url),
            [(b"delegate\n", b"idle-resume-ok"), b"/q\n"],
            args=("--yolo",),
            timeout=8,
        )
        assert_true(code == 0, output)
        assert_true(b"child-running" in output, output)
        assert_true(b"idle-resume-ok" in output, output)
    finally:
        server.close()


def test_queue_then_escape_while_running_command(root, home):
    workspace = root / "steer-run-workspace"
    workspace.mkdir()

    def route(_, body):
        user_text = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "user"
        ]
        if "stop now" in user_text:
            return event({"content": "run-steering-ok"})
        return tool_call("run", {"command": "sleep 30"})

    server = Server([route])
    try:
        code, output = run_pty(
            workspace,
            base_env(home, server.url),
            [
                (b"run slowly\n", b"sleep 30"),
                (b"stop now\n", b"steer:1"),
                (b"\x1b", b"test (default) @ 127.0.0.1"),
                b"/q\n",
            ],
            args=("--yolo",),
            timeout=12,
        )
        assert_true(code == 0, output)
        assert_true(b"steer:1" in output, output)
        assert_true(b"interrupting" in output, output)
        assert_true(b"run-steering-ok" in output, output)
    finally:
        server.close()


def test_slow_command_avoids_poll_round(root, home):
    def verify(_, body):
        results = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        valid = len(results) == 1 and "slow-done" in results[0]
        return event({"content": "slow-ok" if valid else "slow-bad"})

    server = Server(
        [
            tool_call("run", {"command": "sleep 1.2; printf 'slow-done\\n'"}),
            verify,
        ]
    )
    try:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "run slowly")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "slow-ok", result.stdout)
        assert_true(len(server.requests) == 2, len(server.requests))
    finally:
        server.close()


def test_streamed_search_citations(root, home):
    citation = {
        "type": "url_citation",
        "url_citation": {
            "url": "https://example.com/source",
            "title": "Source",
            "content": "search snippet",
        },
    }
    citation_without_usage = {
        "type": "url_citation",
        "url_citation": {
            "url": "https://example.com/legacy",
            "title": "Legacy source",
            "content": "legacy snippet",
        },
    }
    server = Server(
        [
            {
                "choices": [
                    {
                        "delta": {"content": "grounded", "annotations": [citation]},
                        "finish_reason": "stop",
                    }
                ],
                "usage": {
                    "prompt_tokens": 2,
                    "completion_tokens": 1,
                    "server_tool_use": {"web_search_requests": 1},
                },
            },
            {
                "choices": [
                    {
                        "delta": {
                            "content": "legacy",
                            "annotations": [citation_without_usage],
                        },
                        "finish_reason": "stop",
                    }
                ],
                "usage": {"prompt_tokens": 2, "completion_tokens": 1},
            },
        ]
    )
    try:
        result = run_dialog(root, base_env(home, server.url), "probe\nagain\n/trace\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true("grounded\n  ← web_search" in result.stdout, result.stdout)
        assert_true("web_search ×1 · 1 source" in result.stdout, result.stdout)
        assert_true("legacy\n  ← 1 source" in result.stdout, result.stdout)
        assert_true("Sources:" in result.stdout, result.stdout)
        assert_true("https://example.com/source" in result.stdout, result.stdout)
        assert_true("legacy snippet" in result.stdout, result.stdout)
    finally:
        server.close()


def test_search_sources_use_reasoning_style(root, home):
    citation = {
        "type": "url_citation",
        "url_citation": {
            "url": "https://example.com/source",
            "title": "Source",
        },
    }
    server = Server(
        [
            {
                "choices": [
                    {
                        "delta": {
                            "content": "grounded",
                            "annotations": [citation],
                        },
                        "finish_reason": "stop",
                    }
                ],
                "usage": {"completion_tokens": 1},
            }
        ]
    )
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"search\n", b"https://example.com/source"), b"/q\n"],
        )
        assert_true(code == 0, output)
        assert_true(b"\x1b[2mSources:" in output, output)
        assert_true(b"\x1b[2m- <https://example.com/source>" in output, output)
    finally:
        server.close()


def test_headless_debug_session_end(root, home):
    trace = root / "headless-debug.jsonl"
    server = Server(
        [
            event(
                {"content": "ok"},
                usage={
                    "prompt_tokens": 5,
                    "completion_tokens": 2,
                    "prompt_tokens_details": {"cached_tokens": 3},
                },
            )
        ]
    )
    try:
        result = run(
            root,
            base_env(home, server.url),
            f"--debug={trace}",
            "-p",
            "reply",
        )
        assert_true(result.returncode == 0, result.stderr)
        records = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        end = records[-1]
        assert_true(end["event"] == "session_end", end)
        assert_true(end["data"]["reason"] == "headless_complete", end)
        assert_true(end["data"]["usage"]["cache_read"] == 3, end)
    finally:
        server.close()


def test_headless_json_envelope_contains_trace_usage_and_exit(root, home):
    first = tool_call("run", {"command": "printf tool-json"})
    first["usage"] = {
        "prompt_tokens": 7,
        "completion_tokens": 3,
        "completion_tokens_details": {"reasoning_tokens": 1},
        "cost": 0.01,
    }
    server = Server(
        [
            first,
            event(
                {"content": "final-json-answer"},
                usage={"prompt_tokens": 5, "completion_tokens": 2, "cost": 0.02},
            ),
        ]
    )
    try:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            "-p",
            "run a tool",
            "--json",
        )
        assert_true(result.returncode == 0, result.stderr)
        envelope = json.loads(result.stdout)
        assert_true(envelope["schema"] == "uagent.headless.v1", envelope)
        assert_true(envelope["answer"] == "final-json-answer", envelope)
        assert_true(envelope["error"] is None, envelope)
        assert_true(envelope["exit_code"] == 0, envelope)
        assert_true(envelope["usage"]["input"] == 12, envelope)
        assert_true(envelope["usage"]["output"] == 4, envelope)
        assert_true(envelope["usage"]["reasoning"] == 1, envelope)
        assert_true(abs(envelope["usage"]["cost"] - 0.03) < 1e-9, envelope)
        assert_true(envelope["usage"]["cost_reported"], envelope)
        assert_true(envelope["routes"], envelope)
        assert_true(len(envelope["trace"]) == 1, envelope)
        call = envelope["trace"][0]
        assert_true(call["name"] == "run", call)
        assert_true(call["arguments"] == {"command": "printf tool-json"}, call)
        assert_true("tool-json" in call["result"], call)
    finally:
        server.close()


def test_headless_json_error_is_machine_readable(root, home):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    result = run(
        root,
        base_env(home, f"http://127.0.0.1:{port}/v1"),
        "-p",
        "reply",
        "--json",
    )
    assert_true(result.returncode == 1, result.returncode)
    envelope = json.loads(result.stdout)
    assert_true(envelope["exit_code"] == 1, envelope)
    assert_true(envelope["answer"] == "", envelope)
    assert_true("connection error:" in envelope["error"], envelope)
    assert_true(
        set(envelope) == {"schema", "answer", "error", "trace", "usage", "routes", "exit_code"},
        envelope,
    )
    assert_true(not result.stderr, result.stderr)


def test_headless_json_early_error_has_stable_schema(root, home):
    result = run(root, base_env(home, "http://127.0.0.1:1/v1"), "--json", "--bad")
    assert_true(result.returncode == 2, result)
    envelope = json.loads(result.stdout)
    assert_true(
        set(envelope) == {"schema", "answer", "error", "trace", "usage", "routes", "exit_code"},
        envelope,
    )
    assert_true(envelope["routes"] == {}, envelope)


def test_eval_dispatch_uses_uv_and_selected_binary(root, home):
    fake_bin = root / "fake-bin"
    fake_bin.mkdir(exist_ok=True)
    fake_uv = fake_bin / "uv"
    fake_uv.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n", encoding="utf-8")
    fake_uv.chmod(0o755)
    evaluator = root / "eval.py"
    evaluator.write_text("# evaluator fixture\n", encoding="utf-8")
    env = base_env(home, "http://127.0.0.1:1/v1")
    env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
    env["UAGENT_EVAL_SCRIPT"] = str(evaluator)
    result = run(root, env, "eval", "--list-scenarios")
    assert_true(result.returncode == 0, result.stderr)
    arguments = result.stdout.splitlines()
    assert_true(arguments[:4] == ["run", "--no-project", str(evaluator), "--run"], arguments)
    assert_true("--binary" in arguments and "--list-scenarios" in arguments, arguments)


def test_headless_json_stream_emits_lifecycle_events(root, home):
    server = Server(
        [
            tool_call("list_dir", {"path": "."}),
            event(
                {"content": "stream-answer"},
                usage={"prompt_tokens": 4, "completion_tokens": 2, "cost": 0.01},
            ),
        ]
    )
    try:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            "--json-stream",
            "-p",
            "inspect",
        )
        assert_true(result.returncode == 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        assert_true(all(item["schema"] == "uagent.event.v1" for item in records), records)
        types = [item["type"] for item in records]
        assert_true(types[0] == "turn.started", types)
        assert_true("tool.call" in types and "tool.result" in types, types)
        assert_true("usage" in types and types[-1] == "answer", types)
        assert_true(records[-1]["data"]["answer"] == "stream-answer", records[-1])
    finally:
        server.close()


def test_session_budget_stops_before_the_next_call(root, home):
    expensive = tool_call("list_dir", {"path": "."})
    expensive["usage"] = {
        "prompt_tokens": 10,
        "completion_tokens": 2,
        "cost": 0.06,
    }
    server = Server([expensive, event({"content": "too-late"})])
    try:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            "--budget",
            "0.05",
            "--json",
            "-p",
            "inspect",
        )
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 1, envelope)
        assert_true("session cost limit exceeded" in envelope["error"], envelope)
        assert_true(len(server.requests) == 1, server.requests)
    finally:
        server.close()


def test_delegated_session_budget_uses_remaining_allowance(root, home):
    child_requests = []
    parent_requests = []

    def route(_, body):
        is_child = any(
            message.get("role") == "user" and message.get("content") == "child"
            for message in body["messages"]
        )
        if is_child:
            child_requests.append(1)
            response = tool_call("list_dir", {"path": "."})
            response["usage"] = {
                "prompt_tokens": 5,
                "completion_tokens": 1,
                "cost": 0.03,
            }
            return response
        parent_requests.append(1)
        if len(parent_requests) > 1:
            return tool_call("activity_wait", {"wait_ms": 30000})
        response = tool_call("task", {"prompt": "child"})
        response["usage"] = {
            "prompt_tokens": 5,
            "completion_tokens": 1,
            "cost": 0.03,
        }
        return response

    server = Server([route])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "16384"
        result = run(
            root,
            env,
            "--yolo",
            "--budget",
            "0.05",
            "--json",
            "-p",
            "delegate",
            timeout=60,
        )
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 1, envelope)
        assert_true(len(child_requests) == 1, child_requests)
        assert_true(len(server.requests) == 3, server.requests)
        assert_true(envelope["usage"]["cost"] >= 0.06, envelope)
    finally:
        server.close()


def test_budget_marks_missing_provider_cost(root, home):
    server = Server([event({"content": "no-cost"}, usage={"prompt_tokens": 3})])
    try:
        result = run(
            root,
            base_env(home, server.url),
            "--budget",
            "0.01",
            "--json",
            "-p",
            "reply",
        )
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 0, envelope)
        assert_true(envelope["usage"]["cost"] == 0, envelope)
        assert_true(not envelope["usage"]["cost_reported"], envelope)
    finally:
        server.close()


def test_three_tool_failures_emit_one_advisory(root, home):
    trace = root / "failure-advisory.jsonl"

    def verify_advisory(_, body):
        advisories = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "system"
            and "[tool failure advisory]" in message.get("content", "")
        ]
        return event({"content": "advisory-ok" if len(advisories) == 1 else "advisory-bad"})

    server = Server(
        [
            tool_call("run", {"command": "false"}),
            tool_call("run", {"command": "false"}),
            tool_call("run", {"command": "false"}),
            verify_advisory,
        ]
    )
    try:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            f"--debug={trace}",
            "-p",
            "recover from failures",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "advisory-ok", result.stdout)
        records = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        advisories = [record for record in records if record["event"] == "tool_failure_advisory"]
        assert_true(len(advisories) == 1, advisories)
        assert_true(advisories[0]["data"]["consecutive_failures"] == 3, advisories)
    finally:
        server.close()


def test_tool_policy_scopes_schema_and_runtime(root, home):
    marker = root / "tool-policy-marker"

    def request_forbidden(_, body):
        names = function_names(body)
        if names != {"grep", "read_file", "list_dir", "run"}:
            return event({"content": f"bad-schema:{sorted(names)}"})
        return tool_call("run", {"command": f"touch {marker}"})

    def verify_rejected(_, body):
        results = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        rejected = any("not allowed by tool policy" in result for result in results)
        return event({"content": "policy-ok" if rejected else "policy-bad"})

    server = Server([request_forbidden, verify_rejected])
    try:
        env = base_env(home, server.url)
        env.update(
            {
                "UAGENT_TOOL_CAPABILITIES": "inspect",
                "UAGENT_TOOL_ALLOWLIST": json.dumps(["grep", "read_file", "list_dir", "run"]),
                "UAGENT_TOOL_RUN_ALLOWLIST": json.dumps(["python3 slow_analysis.py"]),
                "PYTHONDONTWRITEBYTECODE": "1",
            }
        )
        result = run(root, env, "--yolo", "-p", "inspect only")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "policy-ok", result.stdout)
        assert_true(not marker.exists(), marker)
    finally:
        server.close()


def test_grep_tool_round_trip(root, home):
    workspace = root / "grep-workspace"
    workspace.mkdir()
    (workspace / "one.cpp").write_text("alpha\nproject_wide_symbol\nomega\n", encoding="utf-8")
    (workspace / "ignored.txt").write_text("project_wide_symbol\n", encoding="utf-8")

    def final(_, body):
        result = next(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        valid = (
            "one.cpp" in result
            and "ignored.txt" not in result
            and "project_wide_symbol" in result
            and "alpha" in result
            and "omega" in result
        )
        return event({"content": "grep-ok" if valid else "grep-bad"})

    server = Server(
        [
            tool_call(
                "grep",
                {
                    "pattern": "project_wide_symbol",
                    "path": ".",
                    "glob": "*.cpp",
                    "context": 1,
                },
            ),
            final,
        ]
    )
    try:
        env = base_env(home, server.url)
        env["UAGENT_IMAGE_PROTOCOL"] = "iterm"
        result = run(workspace, env, "-p", "search")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "grep-ok", result.stdout)
        names = function_names(server.requests[0][1])
        assert_true("grep" in names, names)
        assert_true("show_image" not in names, names)
    finally:
        server.close()


def test_small_directory_listing_includes_sources(root, home):
    workspace = root / "small-directory-inspection"
    workspace.mkdir()
    (workspace / "implementation.cc").write_text(
        ("before-xxxxx\n" * 225) + "IMPLEMENTATION_MIDDLE\n" + ("after-xxxxxx\n" * 250),
        encoding="utf-8",
    )
    (workspace / "implementation_test.cc").write_text(
        ("before-xxxxx\n" * 225) + "TEST_MIDDLE\n" + ("after-xxxxxx\n" * 250),
        encoding="utf-8",
    )

    def verify(_, body):
        result = next(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        valid = "IMPLEMENTATION_MIDDLE" in result and "TEST_MIDDLE" in result
        return event(
            {"content": "directory-inspection-ok" if valid else "directory-inspection-bad"}
        )

    server = Server([tool_call("list_dir", {"path": "."}), verify])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "131072"
        result = run(workspace, env, "-p", "inspect this component")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "directory-inspection-ok", result.stdout)
        assert_true(len(server.requests) == 2, len(server.requests))
    finally:
        server.close()


def test_parallel_source_reads_use_one_contiguous_window(root, home):
    workspace = root / "parallel-source-reads"
    workspace.mkdir()
    paths = []
    for name, marker in (("left.cc", "LEFT_MIDDLE"), ("right.cc", "RIGHT_MIDDLE")):
        path = workspace / name
        path.write_text(
            ("before-xxxxx\n" * 450) + marker + "\n" + ("after-xxxxxx\n" * 500),
            encoding="utf-8",
        )
        paths.append(path)

    batch = event(
        {
            "tool_calls": [
                {
                    "index": index,
                    "id": f"call-{index}",
                    "function": {
                        "name": "read_file",
                        "arguments": json.dumps({"path": str(path)}),
                    },
                }
                for index, path in enumerate(paths)
            ]
        },
        finish="tool_calls",
    )

    def verify(_, body):
        combined = "\n".join(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        valid = "LEFT_MIDDLE" in combined and "RIGHT_MIDDLE" in combined
        return event({"content": "source-window-ok" if valid else "source-window-bad"})

    server = Server([batch, verify])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "131072"
        result = run(workspace, env, "-p", "inspect both")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "source-window-ok", result.stdout)
        assert_true(len(server.requests) == 2, len(server.requests))
    finally:
        server.close()


def test_real_headless_error(root, home):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    result = run(root, base_env(home, f"http://127.0.0.1:{port}/v1"), "-p", "reply")
    assert_true(result.returncode == 1, result.returncode)
    assert_true("connection error:" in result.stderr, result.stderr)
    assert_true("produced no answer" not in result.stderr, result.stderr)


def test_project_mcp_trust(root, home):
    workspace = root / "mcp-workspace"
    workspace.mkdir()
    marker = root / "mcp-marker"
    command = (
        f"import pathlib;pathlib.Path({str(marker)!r}).write_text('executed', encoding='utf-8')"
    )
    (workspace / ".mcp.json").write_text(
        json.dumps({"mcpServers": {"probe": {"command": sys.executable, "args": ["-c", command]}}}),
        encoding="utf-8",
    )
    env = base_env(home, "http://127.0.0.1:1/v1")
    denied = run(workspace, env, "-p", "reply")
    assert_true(denied.returncode == 2, denied.stderr)
    assert_true(not marker.exists(), "untrusted MCP command executed")
    allowed = run(workspace, env, "--trust-project-config", "-p", "reply")
    assert_true(allowed.returncode == 1, allowed.returncode)
    assert_true(marker.read_text(encoding="utf-8") == "executed", "trusted MCP did not run")


def test_project_agent_config_trust(root, home):
    workspace = root / "config-workspace"
    (workspace / ".uagent").mkdir(parents=True)
    (home / ".uagent").mkdir(exist_ok=True)
    (home / ".uagent" / ".config").write_text("UAGENT_MODEL=global/model\n", encoding="utf-8")
    (workspace / ".uagent" / ".config").write_text("UAGENT_MODEL=project/model\n", encoding="utf-8")
    server = Server([event({"content": "ok"}), event({"content": "ok"})])
    try:
        env = base_env(home, server.url)
        env.pop("UAGENT_MODEL")
        # Untrusted the workspace file is ignored, but the run still works off
        # the global config instead of failing.
        ignored = run(workspace, env, "-p", "reply")
        assert_true(ignored.returncode == 0, ignored.stderr)
        assert_true("untrusted" in ignored.stderr, ignored.stderr)
        assert_true(server.requests[0][1]["model"] == "global/model", server.requests[0][1])
        trusted = run(workspace, env, "--trust-project-config", "-p", "reply")
        assert_true(trusted.returncode == 0, trusted.stderr)
        assert_true(server.requests[1][1]["model"] == "project/model", server.requests[1][1])
    finally:
        server.close()
        # HOME is shared by every test; leave it as it was found.
        (home / ".uagent" / ".config").unlink(missing_ok=True)


def test_memory_reaches_context_by_scope(root, home):
    workspace = root / "memory-workspace"
    workspace.mkdir()
    project_dir = project_memory_dir(home, workspace)
    project_dir.mkdir(parents=True)
    (project_dir / "build.md").write_text("project-memory-sentinel", encoding="utf-8")
    global_dir = global_memory_dir(home)
    global_dir.mkdir(parents=True, exist_ok=True)
    (global_dir / "style.md").write_text("global-memory-sentinel", encoding="utf-8")
    other = root / "memory-other-workspace"
    other.mkdir()

    def verify(_, body):
        messages = body["messages"]
        memories = next(
            (
                str(message.get("content", ""))
                for message in messages
                if str(message.get("content", "")).startswith(
                    "[memory names only; non-authoritative metadata]"
                )
            ),
            "",
        )
        valid = (
            bool(memories)
            and "global/style" in memories
            and "[always-on behavioral memory; non-authoritative evidence]" in memories
            and "global-memory-sentinel" in memories
            and "project/build" in memories
            and "project-memory-sentinel" not in memories
            and next(
                message.get("role")
                for message in messages
                if str(message.get("content", "")).startswith(
                    "[memory names only; non-authoritative metadata]"
                )
            )
            == "system"
        )
        return event({"content": "memory-ok" if valid else "memory-bad"})

    def verify_isolated(_, body):
        messages = body["messages"]
        memories = " ".join(
            str(message.get("content", ""))
            for message in messages
            if str(message.get("content", "")).startswith(
                "[memory names only; non-authoritative metadata]"
            )
        )
        valid = (
            "global/style" in memories
            and "project/build" not in memories
            and "global-memory-sentinel" in memories
        )
        return event({"content": "isolated-ok" if valid else "isolated-bad"})

    server = Server([verify, verify_isolated])
    try:
        env = base_env(home, server.url)
        result = run(workspace, env, "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "memory-ok", result.stdout)
        elsewhere = run(other, env, "-p", "reply")
        assert_true(elsewhere.returncode == 0, elsewhere.stderr)
        assert_true(elsewhere.stdout.strip() == "isolated-ok", elsewhere.stdout)
    finally:
        server.close()
        # HOME is shared by every test; a global memory would join them all.
        (global_dir / "style.md").unlink(missing_ok=True)


def test_memory_get_accepts_empty_optional_content(root, home):
    workspace = root / "memory-empty-content"
    workspace.mkdir()
    memory_dir = project_memory_dir(home, workspace)
    memory_dir.mkdir(parents=True)
    (memory_dir / "release-window.md").write_text("Friday", encoding="utf-8")

    def verify(_, body):
        result = next(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        return event({"content": "memory-get-ok" if "Friday" in result else "memory-get-bad"})

    server = Server(
        [
            tool_call(
                "memory",
                {"action": "get", "key": "project/release-window", "content": ""},
            ),
            verify,
        ]
    )
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "32768"
        result_run = run(workspace, env, "-p", "recall memory")
        assert_true(result_run.returncode == 0, result_run.stderr)
        assert_true(result_run.stdout.strip() == "memory-get-ok", result_run.stdout)
    finally:
        server.close()


def test_memory_list_and_search_are_progressive(root, home):
    workspace = root / "memory-search-workspace"
    workspace.mkdir()
    memory_dir = project_memory_dir(home, workspace)
    memory_dir.mkdir(parents=True)
    (memory_dir / "release-window.md").write_text(
        "Deploy only on Friday after CI passes.", encoding="utf-8"
    )

    def search(_, body):
        schema = function_tool(body, "memory")["parameters"]
        valid = schema["required"] == ["action"] and {
            "list",
            "search",
        } <= set(schema["properties"]["action"]["enum"])
        return (
            tool_call("memory", {"action": "search", "key": "Friday"})
            if valid
            else event({"content": "memory-search-schema-bad"})
        )

    def finish(_, body):
        result = next(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        return event(
            {
                "content": "memory-search-ok"
                if "project/release-window" in result and "Friday" in result
                else "memory-search-bad"
            }
        )

    server = Server([search, finish])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "32768"
        result = run(workspace, env, "-p", "search memory")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "memory-search-ok", result.stdout)
    finally:
        server.close()


def test_no_memory_hides_index_and_tool(root, home):
    workspace = root / "no-memory-workspace"
    workspace.mkdir()
    project_dir = project_memory_dir(home, workspace)
    project_dir.mkdir(parents=True)
    (project_dir / "project.md").write_text("project-memory-sentinel", encoding="utf-8")
    global_dir = global_memory_dir(home)
    global_dir.mkdir(parents=True, exist_ok=True)
    (global_dir / "global.md").write_text("global-memory-sentinel", encoding="utf-8")

    def verify(_, body):
        text = json.dumps(body)
        names = function_names(body)
        clean = (
            "[memory names only;" not in text
            and "memory-sentinel" not in text
            and "memory" not in names
        )
        return event({"content": "no-memory-ok" if clean else "no-memory-bad"})

    server = Server([verify])
    try:
        result = run(
            workspace,
            base_env(home, server.url),
            "--no-memory",
            "-p",
            "inspect",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "no-memory-ok", result.stdout)
    finally:
        server.close()
        (global_dir / "global.md").unlink(missing_ok=True)


def test_skill_tool_offers_and_opens(root, home):
    workspace = root / "skill-workspace"
    skill = workspace / ".uagent" / "skills" / "demo"
    skill.mkdir(parents=True)
    (skill / "SKILL.md").write_text(
        "---\nname: demo\ndescription: demo-description-sentinel\n---\n\ndemo-body-sentinel\n",
        encoding="utf-8",
    )
    unsupported = workspace / ".uagent" / "skills" / "unsupported"
    unsupported.mkdir()
    (unsupported / "SKILL.md").write_text(
        "---\ndescription: unsupported-sentinel\n"
        "requires-tools: missing-tool\n---\n\nunsupported-body\n",
        encoding="utf-8",
    )

    def offer(_, body):
        functions = {t["function"]["name"]: t["function"] for t in body.get("tools", [])}
        properties = functions.get("skill", {}).get("parameters", {}).get("properties", {})
        valid = (
            "skill" in functions
            and set(properties) == {"name", "query", "arguments"}
            and "enum" not in properties["name"]
            and properties["arguments"].get("maxLength") == 4096
            # Progressive disclosure: metadata rides every request; the body does not.
            and "demo-description-sentinel" in functions["skill"]["description"]
            and "unsupported-sentinel" not in functions["skill"]["description"]
            and "demo-body-sentinel" not in json.dumps(body)
        )
        if not valid:
            return event({"content": "schema-bad"})
        return tool_call("skill", {"query": "demo"})

    def confirm(_, body):
        opened = any("demo-body-sentinel" in str(m.get("content", "")) for m in body["messages"])
        return event({"content": "skill-ok" if opened else "skill-bad"})

    server = Server([offer, confirm])
    try:
        code, output = run_pty(
            workspace,
            base_env(home, server.url),
            [b"reply\n", b"/trace\n", b"\x04"],
            columns=24,
        )
        assert_true(code == 0, output)
        assert_true(b"skill-ok" in output and b"Skills" in output, output)
        assert_true(b"1 available" in output, output)
        assert_true(b"\x1b[1m\xc2\xb5Agent" in output, output)
        assert_true(b"demo" in output, output)
        assert_true(
            output.find(b"demo-body-sentinel") > output.find(b"skill-ok"),
            output,
        )
    finally:
        server.close()


def test_mcp_image_reaches_the_model(root, home):
    """An MCP screenshot has to end up in the model's context, not just on disk.

    A tool result is text-only, so the image travels as an attachment on the
    next request instead.
    """
    workspace = root / "mcp-image"
    workspace.mkdir()
    png = base64.b64encode(SMALL_PNG).decode()
    fake = workspace / "fake_mcp.py"
    fake.write_text(
        "import json, sys\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    method = message.get('method')\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    if method == 'initialize':\n"
        "        result = {'protocolVersion': '2025-11-25', "
        "'capabilities': {'tools': {}}, 'serverInfo': {'name': 'fake', 'version': '1'}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'shot', 'description': 'screenshot', "
        "'inputSchema': {'type': 'object', 'properties': {}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        result = {'content': [{'type': 'image', 'data': PNG, "
        "'mimeType': 'image/png'}]}\n"
        "    else:\n"
        "        result = {}\n"
        "    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': result}), "
        "flush=True)\n".replace("PNG", repr(png)),
        encoding="utf-8",
    )
    (workspace / ".mcp.json").write_text(
        json.dumps({"mcpServers": {"cam": {"command": sys.executable, "args": [str(fake)]}}}),
        encoding="utf-8",
    )

    def verify(_, body):
        tool_text = "".join(
            str(m.get("content", "")) for m in body["messages"] if m.get("role") == "tool"
        )
        parts = [
            part
            for m in body["messages"]
            if isinstance(m.get("content"), list)
            for part in m["content"]
        ]
        got_image = any(p.get("type") == "image_url" for p in parts)
        saved = "mcp image saved" in tool_text and "attached" in tool_text
        # the terminal display used to happen on every call and is now gone
        quiet = "displayed inline" not in tool_text
        return event({"content": "image-ok" if (got_image and saved and quiet) else "image-bad"})

    server = Server([tool_call("cam_shot", {}), verify])
    try:
        env = base_env(home, server.url)
        result = run(workspace, env, "--trust-project-config", "--yolo", "-p", "screenshot")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "image-ok", result.stdout + result.stderr)
    finally:
        server.close()


def test_image_input_rejection_degrades(root, home):
    """Half of OpenRouter's models are text-only and refuse images with a 404.

    The turn has to survive that: drop the picture, keep the path, carry on.
    """
    workspace = root / "image-degrade"
    workspace.mkdir()
    png = workspace / "shot.png"
    png.write_bytes(SMALL_PNG)

    def reject(handler, body):
        parts = [
            part
            for m in body["messages"]
            if isinstance(m.get("content"), list)
            for part in m["content"]
        ]
        if not any(p.get("type") == "image_url" for p in parts):
            return event({"content": "no-image-to-reject"})
        data = json.dumps(
            {"error": {"message": "No endpoints found that support image input", "code": 404}}
        ).encode()
        handler.send_response(404)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(data)))
        handler.end_headers()
        handler.wfile.write(data)
        return None

    def after(_, body):
        messages = body["messages"]
        blob = json.dumps(messages)
        system = "\n".join(
            str(message.get("content", ""))
            for message in messages
            if message.get("role") == "system"
        )
        user = "\n".join(
            str(message.get("content", "")) for message in messages if message.get("role") == "user"
        )
        if (
            '"image_url"' in blob
            or "Image input unavailable" not in system
            or "does not accept image input" in user
            or "withheld" in user
        ):
            return event({"content": "degrade-bad retry"})
        # The capability is dropped for the session, not just for this request:
        # attaching again must be refused rather than resend an image.
        return tool_call("attach", {"path": str(png)})

    def second(_, body):
        results = [str(m.get("content", "")) for m in body["messages"] if m.get("role") == "tool"]
        refused = any("rejected image input" in r for r in results)
        still_no_image = '"image_url"' not in json.dumps(body["messages"])
        return event(
            {"content": "degrade-ok" if (refused and still_no_image) else "degrade-bad second"}
        )

    server = Server([tool_call("attach", {"path": str(png)}), reject, after, second])
    try:
        profile_path = home / ".uagent" / "config" / "routes.json"
        profile_path.parent.mkdir(parents=True, exist_ok=True)
        authority = urllib.parse.urlparse(server.url).netloc
        profile_path.write_text(
            json.dumps(
                {
                    "schema": 3,
                    "routes": {
                        f"{authority}||test|": {
                            "samples": 4,
                            "passing_samples": 4,
                            "pass_rate": 1.0,
                            "certified": True,
                            "scenario_classes": ["analysis", "image"],
                            "certified_at_unix": int(time.time()),
                            "image_support": True,
                        }
                    },
                    "recommendations": {},
                }
            ),
            encoding="utf-8",
        )
        result = run(workspace, base_env(home, server.url), "--yolo", "-p", "look")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "degrade-ok", result.stdout + result.stderr)
        # The notice rides on stdout like the other degradations, which headless
        # sends to /dev/null; the retry itself is what this test pins down.
        assert_true(len(server.requests) == 4, len(server.requests))
        invalidated = json.loads(profile_path.read_text(encoding="utf-8"))["routes"][
            f"{authority}||test|"
        ]
        assert_true(invalidated.get("invalidated_feature") == "image_input", invalidated)
        assert_true(int(invalidated.get("invalidated_at_unix", 0)) > 0, invalidated)
    finally:
        profile_path.unlink(missing_ok=True)
        server.close()


def test_invalid_mcp_config_not_executed(root, home):
    workspace = root / "mcp-invalid-config"
    workspace.mkdir()
    marker = root / "invalid-mcp-marker"
    command = (
        f"import pathlib;pathlib.Path({str(marker)!r}).write_text('executed', encoding='utf-8')"
    )
    (workspace / ".mcp.json").write_text(
        json.dumps(
            {
                "mcpServers": {
                    "invalid": {
                        "command": sys.executable,
                        "args": [1, "-c", command],
                    }
                }
            }
        ),
        encoding="utf-8",
    )
    server = Server([event({"content": "ok"})])
    try:
        result = run(
            workspace,
            base_env(home, server.url),
            "--trust-project-config",
            "-p",
            "reply",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "ok", result.stdout)
        assert_true(not marker.exists(), "invalid MCP server config executed")
    finally:
        server.close()


def test_failed_mcp_server_remains_diagnosable(root, home):
    (home / ".mcp.json").write_text(
        json.dumps({"mcpServers": {"broken": {"command": "/missing/uagent-mcp-server"}}}),
        encoding="utf-8",
    )

    def route(_, body):
        results = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        if results:
            healthy = "broken · error" in results[-1] and "server exited" in results[-1]
            return event({"content": "mcp-diagnostic-ok" if healthy else "mcp-diagnostic-bad"})
        assert_true("mcp_status" in function_names(body), function_names(body))
        return tool_call("mcp_status", {"server": "broken"})

    server = Server([route])
    try:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "diagnose mcp")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "mcp-diagnostic-ok", result.stdout)
    finally:
        server.close()


def test_mcp_tool_round_trip(root, home):
    workspace = root / "mcp-round-trip"
    workspace.mkdir()
    fake = workspace / "fake_mcp.py"
    fake.write_text(
        "import json, sys\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    method = message.get('method')\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    if method == 'initialize':\n"
        "        result = {'protocolVersion': '2025-11-25', "
        "'capabilities': {'tools': {}}, 'serverInfo': {'name': 'fake', 'version': '1'}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'echo', 'description': 'echo text', "
        "'inputSchema': {'type': 'object', 'properties': {'text': {'type': 'string'}}, "
        "'required': ['text']}}]}\n"
        "    elif method == 'tools/call':\n"
        "        text = message.get('params', {}).get('arguments', {}).get('text', '')\n"
        "        result = {'content': [{'type': 'text', 'text': 'mcp:' + text}]}\n"
        "    else:\n"
        "        result = {}\n"
        "    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': result}), "
        "flush=True)\n",
        encoding="utf-8",
    )
    (workspace / ".mcp.json").write_text(
        json.dumps(
            {
                "mcpServers": {
                    "probe": {
                        "command": sys.executable,
                        "args": [str(fake)],
                    }
                }
            }
        ),
        encoding="utf-8",
    )

    def final(_, body):
        result = next(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        return event({"content": "mcp-ok" if "mcp:hello" in result else "mcp-bad"})

    server = Server([tool_call("probe_echo", {"text": "hello"}), final])
    try:
        result = run(
            workspace,
            base_env(home, server.url),
            "--trust-project-config",
            "--yolo",
            "-p",
            "probe",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "mcp-ok", result.stdout)
        names = function_names(server.requests[0][1])
        assert_true("probe_echo" in names, names)
    finally:
        server.close()


def test_mcp_stdio_contract(root, home):
    workspace = root / "mcp-stdio-contract"
    workspace.mkdir()
    server_root = workspace / "server-root"
    server_root.mkdir()
    fake = workspace / "contract_mcp.py"
    fake.write_text(
        "import json, os, sys\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    method = message.get('method')\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    if method == 'initialize':\n"
        "        result = {'protocolVersion': '2025-11-25', "
        "'capabilities': {'tools': {'listChanged': True}}, "
        "'serverInfo': {'name': 'contract', 'version': '1'}}\n"
        "    elif method == 'tools/list':\n"
        "        exact = {'$schema': 'https://json-schema.org/draft/2020-12/schema', "
        "'title': 'Exact', 'type': 'object', "
        "'properties': {'value': {'type': 'string'}}, "
        "'required': ['value'], 'additionalProperties': False}\n"
        "        result = {'tools': ["
        "{'name': 'echo', 'description': 'contract probe', 'inputSchema': exact, "
        "'outputSchema': {'type': 'object', 'required': ['ok']}}, "
        "{'name': 'task_only', 'inputSchema': {'type': 'object'}, "
        "'execution': {'taskSupport': 'required'}}]}\n"
        "    elif method == 'tools/call':\n"
        "        result = {'content': ["
        "{'type': 'text', 'text': 'cwd=' + os.getcwd() + "
        "' env=' + os.environ.get('EXPANDED', '')}, "
        "{'type': 'image', 'data': 'aW1hZ2U=', 'mimeType': 'image/png'}, "
        "{'type': 'audio', 'data': 'YXVkaW8=', 'mimeType': 'audio/wav'}, "
        "{'type': 'resource_link', 'uri': 'file:///contract', 'name': 'contract'}, "
        "{'type': 'resource', 'resource': {'uri': 'file:///embedded', "
        "'text': 'resource-text', 'mimeType': 'text/plain'}}], "
        "'structuredContent': {'ok': True}}\n"
        "    else:\n"
        "        result = {}\n"
        "    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], "
        "'result': result}), flush=True)\n",
        encoding="utf-8",
    )
    (workspace / ".mcp.json").write_text(
        json.dumps(
            {
                "mcpServers": {
                    "contract": {
                        "type": "stdio",
                        "command": sys.executable,
                        "args": [str(fake)],
                        "cwd": "server-root",
                        "env": {"EXPANDED": "prefix-${MCP_TEST_VALUE}-$$"},
                    }
                }
            }
        ),
        encoding="utf-8",
    )

    def final(_, body):
        result = next(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        expected = [
            f"cwd={os.path.realpath(server_root)}",
            "env=prefix-expanded-$",
            "[mcp image saved:",
            "YXVkaW8=",
            "file:///contract",
            "file:///embedded",
            "resource-text",
            "structuredContent",
            '"ok":true',
        ]
        return event(
            {
                "content": "contract-ok"
                if all(value in result for value in expected) and "aW1hZ2U=" not in result
                else "contract-bad"
            }
        )

    server = Server([tool_call("contract_echo", {"value": "hello"}), final])
    try:
        env = base_env(home, server.url)
        env["MCP_TEST_VALUE"] = "expanded"
        result = run(
            workspace,
            env,
            "--trust-project-config",
            "--yolo",
            "-p",
            "probe",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "contract-ok", result.stdout)
        functions = {tool["name"]: tool for tool in function_tools(server.requests[0][1])}
        assert_true("contract_task_only" not in functions, functions.keys())
        schema = functions["contract_echo"]["parameters"]
        assert_true(schema["title"] == "Exact", schema)
        assert_true(schema["additionalProperties"] is False, schema)
        assert_true("$schema" in schema, schema)
    finally:
        server.close()


def test_builtin_chrome_session_modes(root, home):
    workspace = root / "builtin-chrome"
    workspace.mkdir()
    fake_bin = workspace / "bin"
    fake_bin.mkdir()
    invocations = workspace / "npx-invocations"
    screenshot = workspace / "slim-screenshot.png"
    fake = workspace / "fake_chrome_mcp.py"
    fake.write_text(
        "import json, pathlib, sys\n"
        "slim = '--slim' in sys.argv\n"
        f"screenshot = pathlib.Path({str(screenshot)!r})\n"
        "screenshot.write_bytes(b'\\x89PNG\\r\\n\\x1a\\n' + b'\\0' * 32)\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    method = message.get('method')\n"
        "    if method == 'initialize':\n"
        "        result = {'protocolVersion': '2025-11-25', "
        "'capabilities': {'tools': {}}, 'serverInfo': {'name': 'chrome', 'version': '1'}}\n"
        "    elif method == 'tools/list':\n"
        "        if slim:\n"
        "            result = {'tools': ["
        "{'name': 'navigate', 'description': 'navigate', 'inputSchema': {'type': 'object', "
        "'properties': {'url': {'type': 'string'}}, 'required': ['url']}}, "
        "{'name': 'evaluate', 'description': 'evaluate', 'inputSchema': {'type': 'object', "
        "'properties': {'script': {'type': 'string'}}, 'required': ['script']}}, "
        "{'name': 'screenshot', 'description': 'screenshot', "
        "'inputSchema': {'type': 'object'}}]}\n"
        "        else:\n"
        "            result = {'tools': ["
        "{'name': 'list_pages', 'description': 'list pages', "
        "'inputSchema': {'type': 'object', 'additionalProperties': False}}, "
        "{'name': 'evaluate_script', 'description': 'evaluate script', "
        "'inputSchema': {'type': 'object', 'properties': "
        "{'function': {'type': 'string'}}, 'required': ['function']}}, "
        "{'name': 'click', 'description': 'click', 'inputSchema': {'type': 'object', "
        "'properties': {'uid': {'type': 'string'}, "
        "'includeSnapshot': {'type': 'boolean'}}, 'required': ['uid']}}]}\n"
        "    elif method == 'tools/call':\n"
        "        name = message.get('params', {}).get('name')\n"
        "        args = message.get('params', {}).get('arguments', {})\n"
        "        if name == 'navigate':\n"
        "            text = 'Navigated to fixture'\n"
        "        elif name == 'evaluate':\n"
        "            text = json.dumps({'url': 'https://fixture', 'title': 'Fixture', "
        "'text': 'ready', 'controls': []})\n"
        "        elif name == 'screenshot':\n"
        "            text = str(screenshot)\n"
        "        else:\n"
        "            text = json.dumps(args)\n"
        "        result = {'content': [{'type': 'text', 'text': text}]}\n"
        "    else:\n"
        "        result = {'content': [{'type': 'text', 'text': 'ok'}]}\n"
        "    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': result}), "
        "flush=True)\n",
        encoding="utf-8",
    )
    npx = fake_bin / "npx"
    npx.write_text(
        "#!/bin/sh\n"
        f"printf '%s\\n' \"$*\" >> {shlex.quote(str(invocations))}\n"
        f'exec {shlex.quote(sys.executable)} {shlex.quote(str(fake))} "$@"\n',
        encoding="utf-8",
    )
    npx.chmod(0o700)

    def switch(_, body):
        names = function_names(body)
        assert_true("chrome-devtools_list_pages" not in names, names)
        assert_true("chrome_session" in names, names)
        return tool_call("chrome_session", {"mode": "user"})

    def use_slim(_, body):
        names = function_names(body)
        assert_true("chrome-devtools_navigate" in names, names)
        assert_true("chrome-devtools_evaluate" in names, names)
        assert_true("chrome-devtools_screenshot" in names, names)
        assert_true("chrome-devtools_click" not in names, names)
        result = next(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        assert_true(
            result == "User Chrome session selected (slim; page health check passed)", result
        )
        return tool_call("chrome-devtools_navigate", {"url": "https://fixture"})

    def take_screenshot(_, body):
        result = next(
            message["content"]
            for message in reversed(body["messages"])
            if message.get("role") == "tool"
        )
        assert_true("Navigated to fixture" in result, result)
        assert_true("[page state;" in result and '"title": "Fixture"' in result, result)
        return tool_call("chrome-devtools_screenshot", {})

    def switch_full(_, body):
        parts = [
            part
            for message in body["messages"]
            if isinstance(message.get("content"), list)
            for part in message["content"]
        ]
        assert_true(any(part.get("type") == "image_url" for part in parts), parts)
        return tool_call("chrome_session", {"mode": "user", "toolset": "full"})

    def use_full(_, body):
        names = function_names(body)
        assert_true("chrome-devtools_list_pages" in names, names)
        assert_true("chrome-devtools_click" in names, names)
        assert_true("chrome-devtools_evaluate" not in names, names)
        result = next(
            message["content"]
            for message in reversed(body["messages"])
            if message.get("role") == "tool"
        )
        assert_true(
            result == "User Chrome session selected (full; page health check passed)", result
        )
        return tool_call("chrome-devtools_click", {"uid": "7"})

    def final(_, body):
        results = [
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        ]
        return event(
            {
                "content": "chrome-ok"
                if json.loads(results[-1]) == {"uid": "7", "includeSnapshot": True}
                else "chrome-bad"
            }
        )

    server = Server([switch, use_slim, take_screenshot, switch_full, use_full, final])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CHROME_DEVTOOLS"] = "1"
        env["UAGENT_CONTEXT"] = "131072"
        env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
        result = run(workspace, env, "--yolo", "-p", "use my browser")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "chrome-ok", result.stdout)
        calls = invocations.read_text(encoding="utf-8").splitlines()
        assert_true(len(calls) == 2, calls)
        assert_true(all("chrome-devtools-mcp@latest" in call for call in calls), calls)
        assert_true(
            all("--auto-connect" in call and "--isolated" not in call for call in calls), calls
        )
        assert_true("--slim" in calls[0], calls)
        assert_true("--slim" not in calls[1], calls)

    finally:
        server.close()


def test_mcp_tool_list_changed(root, home):
    workspace = root / "mcp-list-changed"
    workspace.mkdir()
    fake = workspace / "changing_mcp.py"
    fake.write_text(
        "import json, sys\n"
        "lists = 0\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    method = message.get('method')\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    if method == 'initialize':\n"
        "        result = {'protocolVersion': '2025-11-25', "
        "'capabilities': {'tools': {'listChanged': True}}, "
        "'serverInfo': {'name': 'changing', 'version': '1'}}\n"
        "    elif method == 'tools/list':\n"
        "        lists += 1\n"
        "        name = 'old' if lists == 1 else 'new'\n"
        "        result = {'tools': [{'name': name, 'inputSchema': "
        "{'type': 'object', 'additionalProperties': False}}]}\n"
        "    elif method == 'tools/call':\n"
        "        name = message.get('params', {}).get('name', '')\n"
        "        if name == 'old':\n"
        "            print(json.dumps({'jsonrpc': '2.0', "
        "'method': 'notifications/tools/list_changed'}), flush=True)\n"
        "        result = {'content': [{'type': 'text', 'text': 'called:' + name}]}\n"
        "    else:\n"
        "        result = {}\n"
        "    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], "
        "'result': result}), flush=True)\n",
        encoding="utf-8",
    )
    (workspace / ".mcp.json").write_text(
        json.dumps(
            {
                "mcpServers": {
                    "changing": {
                        "command": sys.executable,
                        "args": [str(fake)],
                    }
                }
            }
        ),
        encoding="utf-8",
    )

    def refreshed(_, body):
        names = function_names(body)
        assert_true("changing_new" in names, names)
        assert_true("changing_old" not in names, names)
        return tool_call("changing_new", {})

    def final(_, body):
        results = [
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        ]
        return event(
            {
                "content": "refresh-ok"
                if any("called:new" in value for value in results)
                else "refresh-bad"
            }
        )

    server = Server([tool_call("changing_old", {}), refreshed, final])
    try:
        result = run(
            workspace,
            base_env(home, server.url),
            "--trust-project-config",
            "--yolo",
            "-p",
            "probe",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "refresh-ok", result.stdout)
    finally:
        server.close()


def test_user_config_interpolation(root, home):
    config_dir = home / ".uagent"
    config_dir.mkdir(exist_ok=True)
    config = config_dir / ".config"
    config.write_text(
        "credential=bar\nUAGENT_API_KEY=$credential\nUAGENT_BASE_URL=http://127.0.0.1:1/v1\n",
        encoding="utf-8",
    )
    os.chmod(config, 0o600)
    server = Server([event({"content": "ok"})])
    try:
        env = base_env(home, server.url)
        env.pop("UAGENT_BASE_URL")
        # Process endpoint wins when present; rewrite config to the live endpoint.
        config.write_text(
            f"credential=bar\nUAGENT_API_KEY=${{credential}}\nUAGENT_BASE_URL={server.url}\n",
            encoding="utf-8",
        )
        os.chmod(config, 0o644)
        result = run(root, env, "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        auth = server.requests[0][0].get("Authorization")
        assert_true(auth == "Bearer bar", auth)
        assert_true(config.stat().st_mode & 0o777 == 0o600, oct(config.stat().st_mode))

        env["UAGENT_API_KEY"] = "process-wins"
        overridden = run(root, env, "-p", "reply")
        assert_true(overridden.returncode == 0, overridden.stderr)
        auth = server.requests[1][0].get("Authorization")
        assert_true(auth == "Bearer process-wins", auth)
    finally:
        server.close()


def test_model_route_switch(root, home):
    first = Server([event({"content": "wrong-provider"})])

    def switched(_, body):
        valid = (
            body.get("model") == "model-b"
            and body.get("reasoning_effort") == "high"
            and body.get("max_tokens") == 16000
        )
        return event({"content": "route-ok" if valid else "route-bad"})

    second = Server([switched])
    providers = {
        "first": {
            "base_url": first.url,
            "api_key": "key-a",
            "context": 4096,
            "models": {"main": {"id": "model-a", "effort": "low"}},
        },
        "second": {
            "base_url": second.url,
            "api_key": "key-b",
            "models": {"fast": {"id": "model-b", "context": 8192, "effort": "medium"}},
        },
    }
    try:
        env = base_env(home, first.url)
        env["UAGENT_PROVIDERS"] = json.dumps(providers)
        env["UAGENT_MODEL"] = "first/main"
        result = run_dialog(
            root,
            env,
            "/models all\n2\n/effort default\n/effort high\nprobe\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("first/main" in result.stdout and "second/fast" in result.stdout, result.stdout)
        assert_true("effort provider default" in result.stdout, result.stdout)
        assert_true("route-ok" in result.stdout, result.stdout)
        assert_true(not first.requests, first.requests)
        auth = second.requests[0][0].get("Authorization")
        assert_true(auth == "Bearer key-b", auth)
    finally:
        first.close()
        second.close()


def test_route_commands_refresh_session_identity(root, home):
    server = Server(
        [
            event({"content": "first-route"}),
            event({"content": "online-route"}),
            event({"content": "effort-route"}),
        ]
    )
    providers = {
        "proxy": {
            "base_url": server.url,
            "api_key": "proxy-key",
            "protocol": "openrouter",
            "models": {"main": "vendor/model"},
        }
    }
    try:
        env = base_env(home, server.url)
        env["UAGENT_PROVIDERS"] = json.dumps(providers)
        env["UAGENT_MODEL"] = "proxy/main"
        result = run_dialog(
            root,
            env,
            "first\n/online\nsecond\n/effort high\nthird\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("first-route" in result.stdout, result.stdout)
        assert_true("online-route" in result.stdout, result.stdout)
        assert_true("effort-route" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 3, server.requests)
        bodies = [body for _, body in server.requests]
        assert_true(
            [body.get("model") for body in bodies]
            == [
                "vendor/model",
                "vendor/model:online",
                "vendor/model:online",
            ],
            bodies,
        )
        session_ids = [body.get("session_id") for body in bodies]
        assert_true(all(session_ids), session_ids)
        assert_true(len(set(session_ids)) == 3, session_ids)
        assert_true(bodies[2].get("reasoning") == {"effort": "high"}, bodies[2])
    finally:
        server.close()


def test_openrouter_variant_is_scoped_to_openrouter(root, home):
    router = Server(
        [
            event({"content": "nitro-ok"}),
            event({"content": "floor-ok"}),
            event({"content": "exacto-ok"}),
            event({"content": "default-ok"}),
        ]
    )
    generic = Server([event({"content": "generic-ok"})])
    providers = {
        "router": {
            "base_url": router.url,
            "api_key": "router-key",
            "protocol": "openrouter",
            "models": {"main": "vendor/model"},
        },
        "generic": {
            "base_url": generic.url,
            "api_key": "generic-key",
            "models": {"main": "other/model"},
        },
    }
    try:
        env = base_env(home, router.url)
        env["UAGENT_PROVIDERS"] = json.dumps(providers)
        env["UAGENT_MODEL"] = "router/main"
        result = run_dialog(
            root,
            env,
            "/variant\n/variant :nitro\none\n/variant floor\ntwo\n"
            "/variant exacto\nthree\n/variant default\nfour\n"
            "/model generic/main\n/variant nitro\nfive\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("choose default, nitro, floor, or exacto" in result.stdout, result.stdout)
        assert_true("/variant is available only for OpenRouter" in result.stdout, result.stdout)
        for reply in ("nitro-ok", "floor-ok", "exacto-ok", "default-ok", "generic-ok"):
            assert_true(reply in result.stdout, result.stdout)
        router_bodies = [body for _, body in router.requests]
        assert_true(
            [body.get("model") for body in router_bodies]
            == [
                "vendor/model:nitro",
                "vendor/model:floor",
                "vendor/model:exacto",
                "vendor/model",
            ],
            router_bodies,
        )
        assert_true(generic.requests[0][1].get("model") == "other/model", generic.requests)
        session_ids = [body.get("session_id") for body in router_bodies]
        assert_true(all(session_ids) and len(set(session_ids)) == 4, session_ids)
    finally:
        router.close()
        generic.close()


def test_handoff_compacts_then_switches_route(root, home):
    first = Server(
        [
            event({"content": "exploration complete"}),
            event({"content": "distilled handoff state"}),
        ]
    )

    def continued(_, body):
        text = "\n".join(str(message.get("content", "")) for message in body["messages"])
        valid = (
            body.get("model") == "strong-model"
            and "distilled handoff state" in text
            and "exploration complete" not in text
        )
        return event({"content": "handoff-ok" if valid else "handoff-bad"})

    second = Server([continued])
    providers = {
        "cheap": {
            "base_url": first.url,
            "api_key": "cheap-key",
            "models": {"flash": "flash-model"},
        },
        "strong": {
            "base_url": second.url,
            "api_key": "strong-key",
            "models": {"main": "strong-model"},
        },
    }
    try:
        env = base_env(home, first.url)
        env["UAGENT_PROVIDERS"] = json.dumps(providers)
        env["UAGENT_MODEL"] = "cheap/flash"
        result = run_dialog(
            root,
            env,
            "explore\n/handoff strong/main\ncontinue\n/cost\n/q\n",
            timeout=20,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("handoff-ok" in result.stdout, result.stdout)
        assert_true("strong/main" in result.stdout, result.stdout)
        assert_true("total" in result.stdout, result.stdout)
        assert_true(len(first.requests) == 2, first.requests)
        assert_true(len(second.requests) == 1, second.requests)
    finally:
        first.close()
        second.close()


def test_handoff_shadow_keeps_route_and_context(root, home):
    def after_shadow(_, body):
        text = "\n".join(str(message.get("content", "")) for message in body["messages"])
        valid = body.get("model") == "flash-model" and "exploration complete" in text
        return event({"content": "shadow-ok" if valid else "shadow-bad"})

    first = Server(
        [
            event({"content": "exploration complete"}),
            event({"content": "candidate summary"}),
            after_shadow,
        ]
    )
    second = Server([event({"content": "wrong-route"})])
    providers = {
        "cheap": {
            "base_url": first.url,
            "api_key": "cheap-key",
            "models": {"flash": "flash-model"},
        },
        "strong": {
            "base_url": second.url,
            "api_key": "strong-key",
            "models": {"main": "strong-model"},
        },
    }
    try:
        env = base_env(home, first.url)
        env["UAGENT_PROVIDERS"] = json.dumps(providers)
        env["UAGENT_MODEL"] = "cheap/flash"
        env["UAGENT_CHECKPOINT_MODE"] = "shadow"
        result = run_dialog(
            root,
            env,
            "explore\n/handoff strong/main\ncontinue\n/q\n",
            timeout=20,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("shadow-ok" in result.stdout, result.stdout)
        assert_true("route and context unchanged" in result.stdout, result.stdout)
        assert_true(not second.requests, second.requests)
    finally:
        first.close()
        second.close()


def test_dynamic_provider_catalog_and_model(root, home):
    active_catalog = {"data": [{"id": "active-live"}]}
    first = Server(
        [event({"content": "original-route-ok"})],
        get_response=active_catalog,
    )

    def switched(handler, body):
        valid = (
            body.get("model") == "gpt-live"
            and handler.headers.get("Authorization") == "Bearer key-b"
        )
        return event({"content": "dynamic-route-ok" if valid else "dynamic-route-bad"})

    catalog = {"data": [{"id": "gpt-live", "context_length": 16384}]}
    second = Server([switched], get_response=catalog)
    providers = {
        "second": {
            "base_url": second.url,
            "api_key": "key-b",
            "context": 16384,
        }
    }
    try:
        env = base_env(home, first.url)
        env["UAGENT_PROVIDERS"] = json.dumps(providers)
        env["UAGENT_MODEL"] = "active-live"
        catalog_result = run_dialog(
            root,
            env,
            "/models live\n\x1b\nprobe\n/q\n",
        )
        assert_true(catalog_result.returncode == 0, catalog_result.stderr)
        assert_true(
            "searching all model catalogs for live" in catalog_result.stdout,
            catalog_result.stdout,
        )
        assert_true("second/gpt-live" in catalog_result.stdout, catalog_result.stdout)
        assert_true("active-live" in catalog_result.stdout, catalog_result.stdout)
        assert_true("keeping active-live" in catalog_result.stdout, catalog_result.stdout)
        assert_true("original-route-ok" in catalog_result.stdout, catalog_result.stdout)
        assert_true(len(first.requests) == 1, first.requests)

        wildcard = run_dialog(
            root,
            env,
            "/models second/*\n\x1b\n/q\n",
        )
        assert_true(wildcard.returncode == 0, wildcard.stderr)
        assert_true("second/gpt-live" in wildcard.stdout, wildcard.stdout)

        selected = run_dialog(
            root,
            env,
            "/models gpt-live\n1\nprobe\n/q\n",
        )
        assert_true(selected.returncode == 0, selected.stderr)
        assert_true("dynamic-route-ok" in selected.stdout, selected.stdout)
        assert_true(len(second.get_requests) == 3, second.get_requests)

        restart_env = base_env(home, first.url)
        restart_env["UAGENT_PROVIDERS"] = json.dumps(providers)
        restart_env.pop("UAGENT_MODEL")
        restarted = run(root, restart_env, "-p", "probe")
        assert_true(restarted.returncode == 0, restarted.stderr)
        assert_true(restarted.stdout.strip() == "dynamic-route-ok", restarted.stdout)
        assert_true(len(first.requests) == 1, first.requests)
    finally:
        first.close()
        second.close()


def test_local_model_uses_openrouter_web_search_route(root, home):
    def verify_search(handler, body):
        # Slow side requests remain inside the tool call and do not consume a
        # model-driven polling round.
        time.sleep(1.2)
        valid = (
            body.get("model") == "vendor/search:online"
            and handler.headers.get("Authorization") == "Bearer search-key"
        )
        data = json.dumps(
            {
                "choices": [
                    {"message": {"content": "search evidence" if valid else "search misrouted"}}
                ],
                "usage": {"prompt_tokens": 4, "completion_tokens": 2, "cost": 0.02},
            }
        ).encode()
        handler.send_response(200)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(data)))
        handler.end_headers()
        handler.wfile.write(data)

    def verify_tool_result(_, body):
        tool_results = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        valid = any("search evidence" in result for result in tool_results)
        return event({"content": "fallback-search-ok" if valid else "fallback-search-bad"})

    local = Server(
        [
            tool_call("web_search", {"query": "current topic"}),
            verify_tool_result,
        ]
    )
    search = Server([verify_search])
    providers = {
        "codex-local": {
            "base_url": local.url + "/api/v1",
            "api_key": "local-key",
        },
        "openrouter": {"base_url": search.url, "api_key": "search-key"},
    }
    try:
        env = base_env(home, local.url)
        env["UAGENT_MODEL"] = "codex-local/local-model"
        env["UAGENT_API_KEY"] = "local-key"
        env["UAGENT_PROVIDERS"] = json.dumps(providers)
        env["OPENROUTER_MODEL"] = "vendor/search"
        result = run(
            root,
            env,
            "--yolo",
            "--json",
            "-p",
            "research the current topic",
            timeout=15,
        )
        assert_true(result.returncode == 0, result.stderr)
        envelope = json.loads(result.stdout)
        assert_true(envelope["answer"] == "fallback-search-ok", envelope)
        search_authority = urllib.parse.urlparse(search.url).netloc
        assert_true(
            any(
                route.startswith(f"{search_authority}|web_search|") for route in envelope["routes"]
            ),
            envelope,
        )
        assert_true(abs(envelope["usage"]["cost"] - 0.02) < 1e-9, envelope)
        assert_true(len(local.requests) == 2, local.requests)
        assert_true(len(search.requests) == 1, search.requests)
    finally:
        local.close()
        search.close()


def test_local_model_switch_keeps_openrouter_web_search(root, home):
    def verify_search(handler, body):
        valid = (
            body.get("model") == "vendor/search:online"
            and handler.headers.get("Authorization") == "Bearer search-key"
        )
        data = json.dumps(
            {"choices": [{"message": {"content": "switched-search" if valid else "misrouted"}}]}
        ).encode()
        handler.send_response(200)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(data)))
        handler.end_headers()
        handler.wfile.write(data)

    def verify_tool_result(_, body):
        results = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        return event(
            {
                "content": (
                    "switch-search-ok"
                    if any("switched-search" in result for result in results)
                    else "switch-search-bad"
                )
            }
        )

    local = Server(
        [
            tool_call("web_search", {"query": "current topic"}),
            verify_tool_result,
        ]
    )
    search = Server([verify_search])
    providers = {
        "codex-local": {"base_url": local.url, "api_key": "local-key"},
        "openrouter": {"base_url": search.url, "api_key": "search-key"},
    }
    try:
        env = base_env(home, search.url)
        env["UAGENT_MODEL"] = "initial/model"
        env["UAGENT_API_KEY"] = "search-key"
        env["UAGENT_OPENROUTER_COMPATIBLE"] = "1"
        env["UAGENT_PROVIDERS"] = json.dumps(providers)
        env["OPENROUTER_MODEL"] = "vendor/search"
        result = run_dialog(
            root,
            env,
            "/model codex-local/local-model\nresearch the current topic\n/q\n",
            "--yolo",
            timeout=15,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("switch-search-ok" in result.stdout, result.stdout)
        assert_true("switch-search-bad" not in result.stdout, result.stdout)
        assert_true(len(search.requests) == 1, search.requests)
    finally:
        local.close()
        search.close()


def test_local_model_uses_independent_responses_web_search(root, home):
    def verify_tool_result(_, body):
        tool_results = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        valid = any(
            "grounded answer" in result and "https://example.com/source" in result
            for result in tool_results
        )
        return event({"content": "responses-search-ok" if valid else "responses-search-bad"})

    local = Server(
        [
            tool_call("web_search", {"query": "current topic"}),
            verify_tool_result,
        ]
    )
    search = Server(
        [
            {
                "status": "completed",
                "output": [
                    {
                        "type": "web_search_call",
                        "action": {
                            "sources": [
                                {
                                    "url": "https://example.com/source",
                                    "title": "Source",
                                }
                            ]
                        },
                    },
                    {
                        "type": "message",
                        "content": [
                            {
                                "type": "output_text",
                                "text": "grounded answer",
                                "annotations": [
                                    {
                                        "type": "url_citation",
                                        "url": "https://example.com/source",
                                        "title": "Source",
                                    }
                                ],
                            }
                        ],
                    },
                ],
                "usage": {
                    "input_tokens": 12,
                    "output_tokens": 6,
                    "output_tokens_details": {"reasoning_tokens": 1},
                },
            }
        ]
    )
    try:
        env = base_env(home, local.url)
        env.update(
            {
                "UAGENT_WEB_SEARCH_BACKEND": "responses",
                "UAGENT_WEB_SEARCH_URL": search.url,
                "UAGENT_WEB_SEARCH_API_KEY": "search-key",
                "UAGENT_WEB_SEARCH_MODEL": "search-model",
            }
        )
        result = run(
            root,
            env,
            "--yolo",
            "-p",
            "research the current topic",
            timeout=15,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "responses-search-ok", result.stdout)
        assert_true(len(local.requests) == 2, local.requests)
        assert_true(len(search.requests) == 1, search.requests)
        headers, body = search.requests[0]
        assert_true(headers.get("Authorization") == "Bearer search-key", headers)
        assert_true(body.get("model") == "search-model", body)
        assert_true(body.get("tools") == [{"type": "web_search"}], body)
        assert_true(body.get("stream") is False, body)
    finally:
        local.close()
        search.close()


def test_model_preference_survives_restart(root, home):
    first = Server([event({"content": "explicit-model-ok"})])

    def remembered(_, body):
        valid = body.get("model") == "model-b" and body.get("reasoning_effort") == "medium"
        return event({"content": "remembered-model-ok" if valid else "remembered-model-bad"})

    second = Server([remembered])
    providers = {
        "first": {
            "base_url": first.url,
            "api_key": "key-a",
            "context": 4096,
            "models": {"main": {"id": "model-a", "effort": "low"}},
        },
        "second": {
            "base_url": second.url,
            "api_key": "key-b",
            "context": 8192,
            "models": {"fast": {"id": "model-b", "effort": "medium"}},
        },
    }
    try:
        choose_env = base_env(home, first.url)
        choose_env["UAGENT_PROVIDERS"] = json.dumps(providers)
        choose_env["UAGENT_MODEL"] = "first/main"
        chosen = run_dialog(root, choose_env, "/model second/fast\n/q\n")
        assert_true(chosen.returncode == 0, chosen.stderr)

        preference = home / ".uagent" / "config" / "model-preference.json"
        saved = json.loads(preference.read_text(encoding="utf-8"))
        assert_true(saved["selection"] == "second/fast" and saved["route"], saved)
        assert_true(preference.stat().st_mode & 0o777 == 0o600, oct(preference.stat().st_mode))

        restart_env = base_env(home, first.url)
        restart_env["UAGENT_PROVIDERS"] = json.dumps(providers)
        restart_env.pop("UAGENT_MODEL")
        restarted = run(root, restart_env, "-p", "probe")
        assert_true(restarted.returncode == 0, restarted.stderr)
        assert_true(restarted.stdout.strip() == "remembered-model-ok", restarted.stdout)
        assert_true(not first.requests, first.requests)

        override_env = dict(restart_env)
        override_env["UAGENT_MODEL"] = "first/main"
        overridden = run(root, override_env, "-p", "probe")
        assert_true(overridden.returncode == 0, overridden.stderr)
        assert_true(overridden.stdout.strip() == "explicit-model-ok", overridden.stdout)
    finally:
        first.close()
        second.close()


def test_live_model_catalog(root, home):
    catalog = {
        "data": [
            {
                "id": "vendor/alpha",
                "context_length": 131072,
                "reasoning": {
                    "supported_efforts": ["low", "high"],
                    "default_effort": "low",
                },
            },
            {"id": "vendor/beta", "context_length": 32768},
        ]
    }
    server = Server([event({"content": "unused"})], get_response=catalog)
    try:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "/models all\n\x1b\n/models alpha\n1\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("vendor/alpha" in result.stdout, result.stdout)
        assert_true("vendor/beta" in result.stdout, result.stdout)
        assert_true("effort low" in result.stdout, result.stdout)
        assert_true("supports low,high" in result.stdout, result.stdout)
        assert_true("· 1 model" in result.stdout, result.stdout)
        assert_true(server.get_requests == ["/v1/models", "/v1/models"], server.get_requests)
    finally:
        server.close()


def test_model_picker_escape_keeps_current(root, home):
    server = Server(
        [event({"content": "unused"})],
        get_response={"data": [{"id": "current-model"}]},
    )
    try:
        env = base_env(home, server.url)
        env["UAGENT_MODEL"] = "current-model"
        code, output = run_pty(
            root,
            env,
            [(b"/models current\n", b"model #"), b"\x1b", b"/q\n"],
        )
        assert_true(code == 0, output)
        assert_true(b"[1]" in output and b"current-model" in output, output)
        assert_true(b"keeping current-model" in output, output)
        assert_true(not server.requests, server.requests)
    finally:
        server.close()


def tool_call(name, arguments):
    return event(
        {
            "tool_calls": [
                {
                    "index": 0,
                    "id": "call-1",
                    "function": {"name": name, "arguments": json.dumps(arguments)},
                }
            ]
        },
        finish="tool_calls",
    )


def checkpoint_env(home, url, mode):
    env = base_env(home, url)
    env["UAGENT_CHECKPOINT_MODE"] = mode
    env["UAGENT_CHECKPOINT_PCT"] = "1"
    env["UAGENT_CHECKPOINT_URGENT_PCT"] = "2"
    env["UAGENT_AUTO_COMPACT_PCT"] = "0"
    return env


def test_checkpoint_apply(root, home):
    workspace = root / "checkpoint-apply"
    workspace.mkdir()
    (workspace / "state.txt").write_text("durable file state\n", encoding="utf-8")

    def folded(_, body):
        messages = body["messages"]
        checkpoint = next(
            (
                message
                for message in messages
                if isinstance(message.get("content"), str)
                and message["content"].startswith("[checkpoint facts; non-authoritative]")
            ),
            {},
        )
        retained_file = next(
            (
                message
                for message in messages
                if isinstance(message.get("content"), str)
                and message["content"].startswith("[checkpoint file state.txt;")
            ),
            {},
        )
        valid = (
            checkpoint.get("role") == "system"
            and "Objective remains stable; tests passed; no unresolved conditions."
            in checkpoint.get("content", "")
            and checkpoint.get("role") != "user"
            and retained_file.get("role") == "system"
            and "durable file state" in retained_file.get("content", "")
            and messages[-1].get("role") == "user"
            and messages[-1].get("content") == "[priority] third request"
            and not any(
                message.get("role") == "user"
                and message.get("content") in {"first request", "[priority] second request"}
                for message in messages
            )
            and not any(message.get("role") == "tool" for message in messages)
        )
        return event({"content": "checkpoint-apply-ok" if valid else "checkpoint-apply-bad"})

    server = Server(
        [
            event({"content": "first-ok"}),
            tool_call(
                "checkpoint",
                {
                    "state": ("Objective remains stable; tests passed; no unresolved conditions."),
                    "keep_paths": ["state.txt"],
                    "keep_last_n_results": 0,
                },
            ),
            folded,
        ]
    )
    try:
        result = run_dialog(
            workspace,
            checkpoint_env(home, server.url, "apply"),
            "first request\n[priority] second request\n[priority] third request\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("checkpoint-apply-ok" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 3, len(server.requests))
        names = [function_names(body) for _, body in server.requests]
        assert_true("checkpoint" not in names[0], names)
        assert_true("checkpoint" in names[1], names)
        assert_true("checkpoint" not in names[2], names)
    finally:
        server.close()


def test_checkpoint_500k_window(root, home):
    workspace = root / "checkpoint-500k"
    workspace.mkdir()

    def checkpoint_requested(_, body):
        text = "\n".join(
            message.get("content", "")
            for message in body["messages"]
            if isinstance(message.get("content"), str)
        )
        assert_true("[context checkpoint suggested]" in text, text)
        return tool_call(
            "checkpoint",
            {
                "state": (
                    "Objective: validate a 500k context fold. "
                    "Constraint: preserve exact durable facts. "
                    "Validation is still pending."
                ),
                "verbatim": [
                    "astropy__astropy-12907",
                    "_cstack",
                    "rot & (sh1 & sh2)",
                ],
                "keep_last_n_results": 0,
            },
        )

    def folded(_, body):
        messages = body["messages"]
        checkpoint = next(
            (
                message
                for message in messages
                if isinstance(message.get("content"), str)
                and message["content"].startswith("[checkpoint facts; non-authoritative]")
            ),
            {},
        )
        valid = (
            checkpoint.get("role") == "system"
            and "Objective: validate a 500k context fold." in checkpoint.get("content", "")
            and any(
                message.get("role") == "system"
                and message.get("content", "").startswith(
                    "[checkpoint exact literals; non-authoritative]"
                )
                and "rot & (sh1 & sh2)" in message.get("content", "")
                for message in messages
            )
            and messages[-1].get("role") == "user"
            and messages[-1].get("content") == "third 500k request"
            and not any(
                message.get("role") == "user"
                and message.get("content") in {"first 500k request", "second 500k request"}
                for message in messages
            )
        )
        return event({"content": "checkpoint-500k-ok" if valid else "checkpoint-500k-bad"})

    server = Server(
        [
            event(
                {"content": "first-ok"},
                usage={"prompt_tokens": 310000, "completion_tokens": 1000},
            ),
            checkpoint_requested,
            folded,
        ]
    )
    try:
        env = checkpoint_env(home, server.url, "apply")
        env["UAGENT_CONTEXT"] = "500000"
        env["UAGENT_CHECKPOINT_PCT"] = "65"
        env["UAGENT_CHECKPOINT_URGENT_PCT"] = "85"
        result = run_dialog(
            workspace,
            env,
            "first 500k request\nsecond 500k request\nthird 500k request\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("checkpoint-500k-ok" in result.stdout, result.stdout)
    finally:
        server.close()


def test_checkpoint_shadow(root, home):
    workspace = root / "checkpoint-shadow"
    workspace.mkdir()

    def shadowed(_, body):
        messages = body["messages"]
        valid = any(
            message.get("role") == "tool" and "shadow mode" in message.get("content", "")
            for message in messages
        ) and any(
            message.get("role") == "user" and message.get("content") == "first request"
            for message in messages
        )
        return event({"content": "checkpoint-shadow-ok" if valid else "checkpoint-shadow-bad"})

    server = Server(
        [
            event({"content": "first-ok"}),
            tool_call(
                "checkpoint",
                {
                    "state": "Candidate facts; validation is pending.",
                    "keep_last_n_results": 0,
                },
            ),
            shadowed,
        ]
    )
    try:
        result = run_dialog(
            workspace,
            checkpoint_env(home, server.url, "shadow"),
            "first request\nsecond request\nq\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("checkpoint-shadow-ok" in result.stdout, result.stdout)
    finally:
        server.close()


def test_checkpoint_retains_runtime_activity(root, home):
    workspace = root / "checkpoint-activity"
    workspace.mkdir()

    def folded(_, body):
        activity = next(
            (
                message
                for message in body["messages"]
                if isinstance(message.get("content"), str)
                and message["content"].startswith(
                    "[checkpoint runtime activity; non-authoritative]"
                )
            ),
            {},
        )
        content = activity.get("content", "")
        valid = (
            activity.get("role") == "system"
            and '"tool":"write_file"' in content
            and '"path":"note.txt"' in content
            and body["messages"][-1].get("role") == "user"
            and body["messages"][-1].get("content") == "third request"
        )
        return event({"content": "checkpoint-activity-ok" if valid else "checkpoint-activity-bad"})

    server = Server(
        [
            event({"content": "first-ok"}),
            tool_call(
                "write_file",
                {"path": "note.txt", "content": "runtime-owned mutation\n"},
            ),
            tool_call(
                "checkpoint",
                {
                    "state": "The requested note exists and has not been validated.",
                    "keep_last_n_results": 0,
                },
            ),
            folded,
        ]
    )
    try:
        result = run_dialog(
            workspace,
            checkpoint_env(home, server.url, "apply"),
            "first request\nsecond request\nthird request\n/q\n",
            "--yolo",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("checkpoint-activity-ok" in result.stdout, result.stdout)
        assert_true(
            (workspace / "note.txt").read_text(encoding="utf-8") == "runtime-owned mutation\n",
            "write did not complete",
        )
    finally:
        server.close()


def test_checkpoint_preserves_correction(root, home):
    workspace = root / "checkpoint-correction"
    workspace.mkdir()

    def folded(_, body):
        messages = body["messages"]
        checkpoint = next(
            (
                message
                for message in messages
                if isinstance(message.get("content"), str)
                and message["content"].startswith("[checkpoint facts; non-authoritative]")
            ),
            {},
        )
        literals = [
            message
            for message in messages
            if isinstance(message.get("content"), str)
            and message["content"].startswith("[checkpoint exact literals; non-authoritative]")
        ]
        valid = (
            checkpoint.get("role") == "system"
            and "beta is current" in checkpoint.get("content", "")
            and "alpha is obsolete" in checkpoint.get("content", "")
            and any(
                message.get("role") == "system" and '["beta","alpha"]' in message.get("content", "")
                for message in literals
            )
            and messages[-1].get("role") == "user"
            and messages[-1].get("content") == "report the corrected value without tools"
        )
        return event(
            {"content": "checkpoint-correction-ok" if valid else "checkpoint-correction-bad"}
        )

    server = Server(
        [
            event({"content": "first-ok"}),
            tool_call(
                "checkpoint",
                {
                    "state": (
                        "Corrected fact: beta is current; alpha is obsolete. Constraint: no tools."
                    ),
                    "verbatim": ["beta", "alpha"],
                },
            ),
            folded,
        ]
    )
    try:
        result = run_dialog(
            workspace,
            checkpoint_env(home, server.url, "apply"),
            (
                "alpha was initially assumed\n"
                "correction: beta replaces alpha\n"
                "report the corrected value without tools\n"
                "/q\n"
            ),
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("checkpoint-correction-ok" in result.stdout, result.stdout)
    finally:
        server.close()


def test_checkpoint_multiple_folds(root, home):
    workspace = root / "checkpoint-multiple"
    workspace.mkdir()

    def second_fold(_, body):
        messages = body["messages"]
        checkpoints = [
            message
            for message in messages
            if isinstance(message.get("content"), str)
            and message["content"].startswith("[checkpoint facts; non-authoritative]")
        ]
        valid = (
            len(checkpoints) == 1
            and checkpoints[0].get("role") == "system"
            and "second durable state" in checkpoints[0].get("content", "").lower()
            and "first durable state" not in checkpoints[0].get("content", "").lower()
            and messages[-1].get("role") == "user"
            and messages[-1].get("content") == "sixth request"
        )
        return event({"content": "checkpoint-multiple-ok" if valid else "checkpoint-multiple-bad"})

    server = Server(
        [
            event({"content": "first-ok"}),
            tool_call("checkpoint", {"state": "First durable state is stable."}),
            event({"content": "after-first-fold"}),
            event({"content": "debounce-bridge"}),
            tool_call("checkpoint", {"state": "Second durable state is stable."}),
            second_fold,
        ]
    )
    try:
        result = run_dialog(
            workspace,
            checkpoint_env(home, server.url, "apply"),
            (
                "first request\nsecond request\nthird request\nfourth request\n"
                "fifth request\nsixth request\n/q\n"
            ),
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("checkpoint-multiple-ok" in result.stdout, result.stdout)
    finally:
        server.close()


def test_malformed_checkpoint_ends_apply_turn(root, home):
    workspace = root / "checkpoint-malformed"
    workspace.mkdir()
    malformed = event(
        {
            "tool_calls": [
                {
                    "index": 0,
                    "id": "call-malformed",
                    "function": {
                        "name": "checkpoint",
                        "arguments": '{"state":',
                    },
                }
            ]
        },
        finish="tool_calls",
    )

    def not_folded(_, body):
        messages = body["messages"]
        valid = (
            messages[-1].get("role") == "user"
            and messages[-1].get("content") == "third request"
            and not any(
                message.get("role") == "assistant" and message.get("tool_calls")
                for message in messages
            )
            and not any(message.get("role") == "tool" for message in messages)
            and not any(
                isinstance(message.get("content"), str)
                and message["content"].startswith("[checkpoint facts; non-authoritative]")
                for message in messages
            )
        )
        return event(
            {"content": "checkpoint-malformed-ok" if valid else "checkpoint-malformed-bad"}
        )

    server = Server([event({"content": "first-ok"}), malformed, not_folded])
    try:
        result = run_dialog(
            workspace,
            checkpoint_env(home, server.url, "apply"),
            "first request\nsecond request\nthird request\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("checkpoint-malformed-ok" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 3, len(server.requests))
    finally:
        server.close()


def test_checkpoint_rejects_secret_path(root, home):
    workspace = root / "checkpoint-secret"
    workspace.mkdir()
    (workspace / ".env").write_text("TOKEN=do-not-inject\n", encoding="utf-8")

    def rejected(_, body):
        valid = any(
            message.get("role") == "tool"
            and "credential files cannot be reread" in message.get("content", "")
            for message in body["messages"]
        )
        return event({"content": "checkpoint-secret-ok" if valid else "checkpoint-secret-bad"})

    server = Server(
        [
            event({"content": "first-ok"}),
            tool_call(
                "checkpoint",
                {
                    "state": "Safe state; next: continue.",
                    "keep_paths": [".env"],
                },
            ),
            rejected,
        ]
    )
    try:
        result = run_dialog(
            workspace,
            checkpoint_env(home, server.url, "apply"),
            "first request\nsecond request\nq\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("checkpoint-secret-ok" in result.stdout, result.stdout)
        assert_true("do-not-inject" not in json.dumps(server.requests), "secret leaked")
    finally:
        server.close()


def test_project_env_ignored(root, home):
    workspace = root / "env-workspace"
    workspace.mkdir()
    (workspace / ".env").write_text("UAGENT_APPROVAL=yolo\n", encoding="utf-8")

    def final(_, body):
        denied = any(
            message.get("role") == "tool" and "user denied" in message.get("content", "")
            for message in body["messages"]
        )
        return event({"content": "denied" if denied else "executed"})

    server = Server([tool_call("run", {"command": "printf should-not-run"}), final])
    try:
        result = run(workspace, base_env(home, server.url), "-p", "probe")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "denied", result.stdout)
    finally:
        server.close()


def test_external_read_requires_approval(root, home):
    workspace = root / "workspace"
    workspace.mkdir()
    secret = root / "outside-secret"
    secret.write_text("do-not-send", encoding="utf-8")

    def final(_, body):
        content = next(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        safe = "user denied" in content and "do-not-send" not in content
        return event({"content": "safe" if safe else "leaked"})

    server = Server([tool_call("read_file", {"path": str(secret)}), final])
    try:
        result = run(workspace, base_env(home, server.url), "-p", "probe")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "safe", result.stdout)
    finally:
        server.close()


def test_first_event_timeout(root, home):
    def stall(handler, _):
        time.sleep(2)
        try:
            data = sse(event({"content": "late"}))
            handler.send_response(200)
            handler.send_header("Content-Length", str(len(data)))
            handler.end_headers()
            handler.wfile.write(data)
        except BrokenPipeError:
            pass

    server = Server([stall, stall, stall])
    try:
        env = base_env(home, server.url)
        env["UAGENT_FIRST_EVENT_TIMEOUT"] = "1"
        started = time.monotonic()
        result = run(root, env, "-p", "probe")
        elapsed = time.monotonic() - started
        assert_true(result.returncode == 1, result.returncode)
        assert_true("no event within 1s" in result.stderr, result.stderr)
        assert_true(len(server.requests) == 3, server.requests)
        assert_true(3.0 < elapsed < 6.5, elapsed)
    finally:
        server.close()


def test_response_size_limit(root, home):
    server = Server([event({"content": "x" * 4096})])
    try:
        env = base_env(home, server.url)
        env["UAGENT_RESPONSE_BYTES"] = "256"
        result = run(root, env, "-p", "probe")
        assert_true(result.returncode == 1, result.returncode)
        assert_true("response exceeded 256 bytes" in result.stderr, result.stderr)
    finally:
        server.close()


def test_turn_cost_limit(root, home):
    server = Server(
        [
            event(
                {"content": "too expensive"},
                usage={"prompt_tokens": 1, "completion_tokens": 1, "cost": 2.0},
            )
        ]
    )
    try:
        env = base_env(home, server.url)
        env["UAGENT_MAX_TURN_COST"] = "1.0"
        result = run(root, env, "-p", "probe")
        assert_true(result.returncode == 1, result.returncode)
        assert_true("turn cost limit exceeded" in result.stderr, result.stderr)
    finally:
        server.close()


def test_repeated_tool_guard(root, home):
    repeated = tool_call("read_file", {"path": "missing"})
    server = Server([repeated, repeated, repeated, repeated])
    try:
        result = run(root, base_env(home, server.url), "-p", "probe")
        assert_true(result.returncode == 1, result.returncode)
        assert_true("repeated the same tool call" in result.stderr, result.stderr)
        assert_true(len(server.requests) == 4, len(server.requests))
    finally:
        server.close()


def test_repeated_tool_guard_keeps_history_valid(root, home):
    repeated = tool_call("read_file", {"path": "missing"})

    def after_abort(_, body):
        # the aborted turn must still leave its tool call answered before the
        # next real user message (runtime notes may follow it)
        notes = ("[environment:", "[context checkpoint")
        real = [m for m in body["messages"] if not str(m.get("content", "")).startswith(notes)]
        index = next((i for i, m in enumerate(real) if m.get("content") == "continue"), 0)
        valid = index > 0 and real[index - 1].get("role") == "tool"
        return event({"content": "history-ok" if valid else "history-bad"})

    server = Server([repeated, repeated, repeated, repeated, after_abort])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "16384"
        result = run_dialog(
            root,
            env,
            "probe\ncontinue\n/q\n",
            "--yolo",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("history-ok" in result.stdout, result.stdout)
    finally:
        server.close()


def test_interleaved_tool_calls_reset_guard(root, home):
    first = tool_call("read_file", {"path": "missing-a"})
    second = tool_call("read_file", {"path": "missing-b"})
    server = Server(
        [first, second, first, second, first, second, first, event({"content": "done"})]
    )
    try:
        result = run(root, base_env(home, server.url), "-p", "probe")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "done", result.stdout)
        # A transport-level retry can repeat one already accepted mock request
        # on slower runners without changing the guard behavior under test.
        assert_true(len(server.requests) in (8, 9), len(server.requests))
    finally:
        server.close()


def test_midturn_compaction_preserves_progress_and_usage(root, home):
    trace = root / "midturn-compact.jsonl"
    source = root / "midturn-source.txt"
    source.write_text("RAW-TOOL-RESULT-" + "x" * 7800, encoding="utf-8")
    output = root / "midturn-output.txt"

    first = event(
        {
            "tool_calls": [
                {
                    "index": 0,
                    "id": "midturn-call",
                    "function": {
                        "name": "read_file",
                        "arguments": json.dumps({"path": str(source)}),
                    },
                }
            ]
        },
        finish="tool_calls",
        usage={"prompt_tokens": 2000, "completion_tokens": 10},
    )

    def compact(_, body):
        prompt = body["messages"][-1].get("content", "")
        text = "\n".join(str(message.get("content", "")) for message in body["messages"])
        valid = (
            str(prompt).startswith("Summarize for a fresh context:")
            and not body.get("tools")
            and "RAW-TOOL-RESULT-" in text
        )
        return event(
            {"content": "MIDTURN-SUMMARY" if valid else "BAD-SUMMARY"},
            usage={"prompt_tokens": 20, "completion_tokens": 5},
        )

    def finish(_, body):
        messages = body["messages"]
        text = "\n".join(str(message.get("content", "")) for message in messages)
        valid = (
            "Prior context:\nMIDTURN-SUMMARY" in text
            and "[model-generated context summary; non-authoritative]" in text
            and "Summarize for a fresh context:" not in text
            and "RAW-TOOL-RESULT-" not in text
            and not any(message.get("role") == "tool" for message in messages)
            and not any(message.get("tool_calls") for message in messages)
            and bool(body.get("tools"))
            and any(
                message.get("role") == "system"
                and "MIDTURN-SUMMARY" in str(message.get("content", ""))
                for message in messages
            )
            and any(
                message.get("role") == "user" and message.get("content") == "inspect"
                for message in messages
            )
            and any(
                message.get("role") == "system"
                and str(message.get("content", "")).startswith("[environment:")
                for message in messages
            )
            and len(messages) < len(server.requests[1][1]["messages"])
            and len(json.dumps(messages)) < len(json.dumps(server.requests[1][1]["messages"]))
            and len(json.dumps(body)) < len(json.dumps(server.requests[1][1]))
        )
        return event(
            {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "write-after-compact",
                        "function": {
                            "name": "write_file",
                            "arguments": json.dumps(
                                {
                                    "path": str(output),
                                    "content": (
                                        "midturn-compact-ok" if valid else "midturn-compact-bad"
                                    ),
                                }
                            ),
                        },
                    }
                ]
            },
            finish="tool_calls",
            usage={"prompt_tokens": 30, "completion_tokens": 7},
        )

    def final(_, body):
        tool_result = next(
            (
                message.get("content", "")
                for message in body["messages"]
                if message.get("role") == "tool"
                and message.get("tool_call_id") == "write-after-compact"
            ),
            "",
        )
        valid = output.read_text(encoding="utf-8") == "midturn-compact-ok"
        valid = valid and "error:" not in tool_result
        return event(
            {"content": "midturn-finished-ok" if valid else "midturn-finished-bad"},
            usage={"prompt_tokens": 40, "completion_tokens": 8},
        )

    server = Server([first, compact, finish, final])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "8000"
        env["UAGENT_MAX_TOKENS"] = "512"
        env["UAGENT_AUTO_COMPACT_PCT"] = "40"
        env["UAGENT_CHECKPOINT_MODE"] = "off"
        env["UAGENT_TOOL_RESULT_CHARS"] = "8000"
        result = run(
            root,
            env,
            "--yolo",
            f"--debug={trace}",
            "-p",
            "inspect",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("midturn-finished-ok" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 4, server.requests)
        records = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        turn_end = next(record for record in records if record["event"] == "turn_end")
        assert_true(turn_end["data"]["usage"]["input"] == 2090, turn_end)
        assert_true(turn_end["data"]["usage"]["output"] == 30, turn_end)
        folds = [record for record in records if record["event"] == "midturn_compact"]
        assert_true(len(folds) == 1, folds)
    finally:
        server.close()


def test_failed_midturn_compaction_keeps_tool_history(root, home):
    source = root / "midturn-fallback-source.txt"
    source.write_text("RAW-FALLBACK-" + "x" * 7800, encoding="utf-8")
    first = event(
        {
            "tool_calls": [
                {
                    "index": 0,
                    "id": "midturn-call",
                    "function": {
                        "name": "read_file",
                        "arguments": json.dumps({"path": str(source)}),
                    },
                }
            ]
        },
        finish="tool_calls",
        usage={"prompt_tokens": 2000, "completion_tokens": 10},
    )

    def finish(_, body):
        messages = body["messages"]
        assistant = [
            message
            for message in messages
            if message.get("role") == "assistant" and message.get("tool_calls")
        ]
        results = [
            message
            for message in messages
            if message.get("role") == "tool" and message.get("tool_call_id") == "midturn-call"
        ]
        leaked_prompt = any(
            str(message.get("content", "")).startswith("Summarize for a fresh context:")
            for message in messages
        )
        valid = len(assistant) == 1 and len(results) == 1 and not leaked_prompt
        return event({"content": "compact-fallback-ok" if valid else "compact-fallback-bad"})

    server = Server([first, event(), finish])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "8000"
        env["UAGENT_MAX_TOKENS"] = "512"
        env["UAGENT_AUTO_COMPACT_PCT"] = "40"
        env["UAGENT_CHECKPOINT_MODE"] = "off"
        env["UAGENT_TOOL_RESULT_CHARS"] = "8000"
        result = run(root, env, "--yolo", "-p", "inspect")
        assert_true(result.returncode == 0, result.stderr)
        assert_true("compact-fallback-ok" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 3, server.requests)
    finally:
        server.close()


def test_midturn_compaction_rechecks_cost_before_continuing(root, home):
    source = root / "midturn-cost-source.txt"
    source.write_text("RAW-COST-" + "x" * 7800, encoding="utf-8")
    first = event(
        {
            "tool_calls": [
                {
                    "index": 0,
                    "id": "cost-call",
                    "function": {
                        "name": "read_file",
                        "arguments": json.dumps({"path": str(source)}),
                    },
                }
            ]
        },
        finish="tool_calls",
        usage={"prompt_tokens": 2000, "completion_tokens": 10},
    )
    compact = event(
        {"content": "Costly summary."},
        usage={"prompt_tokens": 20, "completion_tokens": 5, "cost": 2.0},
    )
    server = Server([first, compact, event({"content": "must-not-run"})])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "8000"
        env["UAGENT_MAX_TOKENS"] = "512"
        env["UAGENT_AUTO_COMPACT_PCT"] = "40"
        env["UAGENT_CHECKPOINT_MODE"] = "off"
        env["UAGENT_TOOL_RESULT_CHARS"] = "8000"
        env["UAGENT_MAX_TURN_COST"] = "1"
        result = run(root, env, "--yolo", "-p", "inspect")
        assert_true(result.returncode == 1, result.returncode)
        assert_true("turn cost limit exceeded" in result.stderr, result.stderr)
        assert_true("must-not-run" not in result.stdout, result.stdout)
        assert_true(len(server.requests) == 2, server.requests)
    finally:
        server.close()


def test_subagent_auto_join_continues_turn(root, home):
    def route(_, body):
        messages = body["messages"]
        if any(
            message.get("role") == "user" and message.get("content") == "child"
            for message in messages
        ):
            time.sleep(2)
            return event({"content": "child-result"})
        has_result = any(
            isinstance(message.get("content"), str) and "[Background result:" in message["content"]
            for message in messages
        )
        if has_result:
            return event({"content": "late-task-ok"})
        if any("[started] task id " in str(message.get("content", "")) for message in messages):
            return tool_call("activity_wait", {"wait_ms": 30000})
        return tool_call("task", {"prompt": "child"})

    server = Server([route])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "16384"
        env["UAGENT_FIRST_EVENT_TIMEOUT"] = "4"
        env["UAGENT_STREAM_IDLE_TIMEOUT"] = "4"
        result = run(root, env, "--yolo", "-p", "delegate", timeout=8)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "late-task-ok", result.stdout)
        request_summary = [
            [
                (message.get("role"), str(message.get("content", ""))[:80])
                for message in body.get("messages", [])
            ]
            for _, body in server.requests
        ]
        assert_true(len(server.requests) == 4, request_summary)
    finally:
        server.close()


def test_subagent_foreground_returns_result_without_wait_round(root, home):
    def route(_, body):
        messages = body["messages"]
        if any(
            message.get("role") == "user" and message.get("content") == "child"
            for message in messages
        ):
            time.sleep(0.3)
            return event({"content": "foreground-child-result"})
        tool_results = [
            str(message.get("content", "")) for message in messages if message.get("role") == "tool"
        ]
        if any("foreground-child-result" in result for result in tool_results):
            direct = all("[started] task id " not in result for result in tool_results)
            return event({"content": "foreground-task-ok" if direct else "foreground-task-bad"})
        task = function_tool(body, "task")
        background = task["parameters"]["properties"]["background"]
        assert_true(background["type"] == "boolean", background)
        assert_true("final result directly" in background["description"], background)
        return tool_call("task", {"prompt": "child", "background": False})

    server = Server([route])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "16384"
        result = run(root, env, "--yolo", "-p", "delegate", timeout=8)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "foreground-task-ok", result.stdout)
        parent_requests = [
            body
            for _, body in server.requests
            if any(
                message.get("role") == "user" and message.get("content") == "delegate"
                for message in body["messages"]
            )
        ]
        assert_true(len(parent_requests) == 2, len(parent_requests))
    finally:
        server.close()


def test_subagent_headless_resumes_on_background_completion(root, home):
    def route(_, body):
        messages = body["messages"]
        if any(
            message.get("role") == "user" and message.get("content") == "child"
            for message in messages
        ):
            time.sleep(0.5)
            return event({"content": "background-child-result"})
        combined = "\n".join(str(message.get("content", "")) for message in messages)
        if "[Background result: task id " in combined:
            harness_wake = any(
                message.get("role") == "system"
                and str(message.get("content", "")).startswith("[harness continuation:")
                for message in messages
            )
            return event({"content": "headless-wake-ok" if harness_wake else "headless-wake-bad"})
        if "[started] task id " in combined:
            # Yield without waiting. Headless orchestration must keep the child
            # alive and initiate a harness-origin continuation on completion.
            return event({"content": "child-still-running"})
        return tool_call("task", {"prompt": "child"})

    server = Server([route])
    trace = root / "headless-background-wake.jsonl"
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "16384"
        result = run(
            root,
            env,
            "--yolo",
            f"--debug={trace}",
            "-p",
            "delegate",
            timeout=8,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "headless-wake-ok", result.stdout)
        origins = [
            event["data"]["origin"]
            for event in map(json.loads, trace.read_text().splitlines())
            if event["event"] == "turn_start"
        ]
        assert_true(origins == ["user", "harness"], origins)
    finally:
        server.close()


def test_no_memory_propagates_to_full_subagent(root, home):
    workspace = root / "no-memory-subagent"
    workspace.mkdir()
    project_dir = project_memory_dir(home, workspace)
    project_dir.mkdir(parents=True)
    (project_dir / "hidden.md").write_text("subagent-memory-sentinel", encoding="utf-8")

    def route(_, body):
        messages = body["messages"]
        is_child = any(
            message.get("role") == "user" and message.get("content") == "child"
            for message in messages
        )
        if is_child:
            names = function_names(body)
            text = json.dumps(messages)
            clean = "memory" not in names and "[memory names only;" not in text
            return event({"content": "child-clean" if clean else "child-contaminated"})
        if any("[Background result:" in str(message.get("content", "")) for message in messages):
            clean = "child-clean" in json.dumps(messages)
            return event({"content": "no-memory-child-ok" if clean else "no-memory-child-bad"})
        if any("[started] task id " in str(message.get("content", "")) for message in messages):
            return tool_call("activity_wait", {"wait_ms": 30000})
        return tool_call("task", {"prompt": "child", "mode": "full"})

    server = Server([route])
    try:
        result = run(
            workspace,
            base_env(home, server.url),
            "--no-memory",
            "--yolo",
            "-p",
            "delegate",
            timeout=60,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "no-memory-child-ok", result.stdout)
    finally:
        server.close()


def test_parallel_subagents_auto_join(root, home):
    children_lock = threading.Lock()
    active_children = 0
    max_active_children = 0

    def route(_, body):
        nonlocal active_children, max_active_children
        messages = body["messages"]
        child = next(
            (
                message.get("content")
                for message in messages
                if message.get("role") == "user"
                and message.get("content") in {"child-a", "child-b"}
            ),
            None,
        )
        if child:
            with children_lock:
                active_children += 1
                max_active_children = max(max_active_children, active_children)
            try:
                time.sleep(1)
                return event({"content": f"{child}-result"})
            finally:
                with children_lock:
                    active_children -= 1
        combined = "\n".join(str(message.get("content", "")) for message in messages)
        if "child-a-result" in combined and "child-b-result" in combined:
            return event({"content": "parallel-task-ok"})
        if "[started] task id " in combined:
            return tool_call("activity_wait", {"wait_ms": 30000})
        return event(
            {
                "tool_calls": [
                    {
                        "index": index,
                        "id": f"task-{index}",
                        "function": {
                            "name": "task",
                            "arguments": json.dumps({"prompt": child_prompt}),
                        },
                    }
                    for index, child_prompt in enumerate(("child-a", "child-b"))
                ]
            },
            finish="tool_calls",
        )

    server = Server([route])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "16384"
        result = run(
            root,
            env,
            "--yolo",
            "-p",
            "delegate twice",
            timeout=8,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "parallel-task-ok", result.stdout)
        assert_true(max_active_children == 2, max_active_children)
        parent_requests = [
            body
            for _, body in server.requests
            if any(
                message.get("role") == "user" and message.get("content") == "delegate twice"
                for message in body["messages"]
            )
        ]
        lifecycle = {"get_task_output", "wait_tasks", "kill_task"}
        assert_true(3 <= len(parent_requests) <= 4, len(parent_requests))
        for request in parent_requests:
            names = function_names(request)
            assert_true("task" in names, names)
            assert_true(not names.intersection(lifecycle), names)
            task_schema = function_tool(request, "task")
            assert_true(
                {"prompt", "model"}.issubset(task_schema["parameters"]["properties"]),
                task_schema,
            )
    finally:
        server.close()


def test_subagent_deadline_reaps_child(root, home):
    def route(_, body):
        messages = body["messages"]
        if any(
            message.get("role") == "user" and message.get("content") == "slow-child"
            for message in messages
        ):
            time.sleep(3)
            return event({"content": "too-late"})
        if any(
            "[Background result: task id " in str(message.get("content", ""))
            for message in messages
        ):
            return event({"content": "deadline-cleanup-ok"})
        return tool_call("task", {"prompt": "slow-child"})

    server = Server([route])
    trace = root / "subagent-deadline.jsonl"
    try:
        env = base_env(home, server.url)
        env["UAGENT_MAX_TURN_SECONDS"] = "1"
        started = time.time()
        run(
            root,
            env,
            "--yolo",
            f"--debug={trace}",
            "-p",
            "delegate slowly",
            timeout=8,
        )
        elapsed = time.time() - started
        assert_true(elapsed < 2.5, f"turn deadline did not stop delegation: {elapsed:.1f}s")
        events = [json.loads(line) for line in trace.read_text().splitlines()]
        started_result = next(
            event["data"]["result"]
            for event in events
            if event["event"] == "tool_result"
            and event["data"]["name"] == "task"
            and event["data"]["result"].startswith("[started] task id ")
        )
        task_pid = int(started_result.split("[started] task id ", 1)[1].split(";", 1)[0])
        try:
            os.kill(task_pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError(f"delegated child {task_pid} survived turn deadline")
    finally:
        server.close()


def test_subagent_interrupt_reaps_child(root, home):
    """A soft interrupt during the tool batch must not orphan delegation."""
    batch = event(
        {
            "tool_calls": [
                {
                    "index": 0,
                    "id": "call-task",
                    "function": {
                        "name": "task",
                        "arguments": json.dumps({"prompt": "slow-child"}),
                    },
                },
                {
                    "index": 1,
                    "id": "call-run",
                    "function": {
                        "name": "run",
                        "arguments": json.dumps({"command": "sleep 5"}),
                    },
                },
            ]
        },
        finish="tool_calls",
    )

    def route(_, body):
        messages = body["messages"]
        if any(
            message.get("role") == "user" and message.get("content") == "slow-child"
            for message in messages
        ):
            time.sleep(5)
            return event({"content": "too-late"})
        if any(message.get("role") == "tool" for message in messages):
            return event({"content": "unexpected-continuation"})
        return batch

    server = Server([route])
    trace = root / "subagent-interrupt.jsonl"
    process = None
    try:
        process = subprocess.Popen(
            [
                str(BINARY),
                "--yolo",
                f"--debug={trace}",
                "-p",
                "delegate then run",
            ],
            cwd=root,
            env=base_env(home, server.url),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        task_pid = None
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline and task_pid is None:
            if trace.exists():
                for line in trace.read_text().splitlines():
                    event_data = json.loads(line)
                    data = event_data.get("data", {})
                    result = data.get("result", "")
                    if (
                        event_data.get("event") == "tool_result"
                        and data.get("name") == "task"
                        and result.startswith("[started] task id ")
                    ):
                        task_pid = int(result.split("[started] task id ", 1)[1].split(";", 1)[0])
                        break
            time.sleep(0.02)
        assert_true(task_pid is not None, "delegated child did not start")
        process.send_signal(signal.SIGINT)
        process.communicate(timeout=8)
        try:
            os.kill(task_pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError(f"delegated child {task_pid} survived interrupt")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        server.close()


def test_parallel_run_overlaps(root, home):
    """`run` is parallel_safe: independent commands must overlap, not queue."""
    sleep, count = 3, 4
    batch = event(
        {
            "tool_calls": [
                {
                    "index": i,
                    "id": f"call-{i}",
                    "function": {
                        "name": "run",
                        "arguments": json.dumps({"command": f"sleep {sleep}; echo done{i}"}),
                    },
                }
                for i in range(count)
            ]
        },
        finish="tool_calls",
    )
    server = Server([batch, event({"content": "parallel-run-ok"})])
    try:
        started = time.time()
        result = run(root, base_env(home, server.url), "--yolo", "-p", "go", timeout=90)
        elapsed = time.time() - started
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "parallel-run-ok", result.stdout)
        # serial would be count*sleep; allow generous slack for spawn overhead
        assert_true(elapsed < sleep * count * 0.7, f"{elapsed:.1f}s for {count}x{sleep}s")
    finally:
        server.close()


def test_parallel_tool_results_share_model_budget(root, home):
    """Large parallel source reads share one read-sized batch window."""
    workspace = root / "parallel-result-budget"
    workspace.mkdir()
    paths = []
    for index, content in enumerate(("a" * 12000, "b" * 12000, "c" * 12000, "small")):
        path = workspace / f"{index}.txt"
        path.write_text(content, encoding="utf-8")
        paths.append(path)

    batch = event(
        {
            "tool_calls": [
                {
                    "index": index,
                    "id": f"call-{index}",
                    "function": {
                        "name": "read_file",
                        "arguments": json.dumps({"path": str(path)}),
                    },
                }
                for index, path in enumerate(paths)
            ]
        },
        finish="tool_calls",
    )

    def verify(_, body):
        results = [
            (message["tool_call_id"], message["content"])
            for message in body["messages"]
            if message.get("role") == "tool"
        ]
        valid = (
            [call_id for call_id, _ in results] == [f"call-{index}" for index in range(4)]
            and all(content for _, content in results)
            and sum(len(content) for _, content in results) <= 34816
            and all(len(content) < 12000 for _, content in results[:3])
            and "small" in results[3][1]
        )
        return event({"content": "batch-budget-ok" if valid else "batch-budget-bad"})

    server = Server([batch, verify])
    try:
        trace = workspace / "trace.jsonl"
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "131072"
        env["UAGENT_TOOL_RESULT_CHARS"] = "8000"
        result = run(workspace, env, "--yolo", f"--debug={trace}", "-p", "read all")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "batch-budget-ok", result.stdout)
        events = [json.loads(line) for line in trace.read_text().splitlines()]
        capped = next(event["data"] for event in events if event["event"] == "tool_batch_capped")
        assert_true(capped["original_chars"] > capped["model_chars"], capped)
        assert_true(capped["model_chars"] <= 34816, capped)
    finally:
        server.close()


def test_subagent_receives_budget(root, home):
    """Children get their own step budget, so one flailing subagent cannot spend
    the whole turn. With a 1-step budget the child must stop after one request."""
    child_requests = []

    def route(_, body):
        messages = body["messages"]
        is_child = any(m.get("role") == "user" and m.get("content") == "child" for m in messages)
        if is_child:
            assert_true(body.get("model") == "child-model", body.get("model"))
            names = function_names(body)
            assert_true("write_file" not in names, names)
            assert_true("edit_file" not in names, names)
            assert_true("memory" not in names, names)
            assert_true("run_python" in names, names)
            assert_true("task" in names, names)
            child_requests.append(1)
            # the child would loop forever if its step budget were not enforced
            return tool_call("list_dir", {"path": "."})
        combined = "\n".join(str(m.get("content", "")) for m in messages)
        if "[Background result:" in combined:
            return event({"content": "budget-ok"})
        if "[started] task id " in combined:
            return tool_call("activity_wait", {"wait_ms": 30000})
        task = function_tool(body, "task")
        assert_true("provider" not in task["parameters"]["properties"], task)
        assert_true(
            task["parameters"]["properties"]["mode"]["enum"] == ["lean", "full"],
            task,
        )
        return tool_call("task", {"prompt": "child"})

    server = Server([route])
    try:
        env = base_env(home, server.url)
        env["UAGENT_PROVIDERS"] = json.dumps(
            {
                "codex-local": {
                    "base_url": server.url,
                    "api_key": "local-key",
                    "models": {"flash": "child-model"},
                }
            }
        )
        env["UAGENT_TASK_MODEL"] = "codex-local/flash"
        env["UAGENT_SUBAGENT_MAX_STEPS"] = "1"
        result = run(root, env, "--yolo", "-p", "delegate", timeout=60)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(len(child_requests) == 1, f"child made {len(child_requests)} requests")
    finally:
        server.close()


def test_subagent_uses_selected_model_route(root, home):
    child_output = root / "delegated-by-child.txt"

    def child_action(_, body):
        assert_true(body.get("model") == "child-model", body.get("model"))
        assert_true("reasoning" in body and "stream_options" not in body, body)
        names = function_names(body)
        assert_true({"write_file", "edit_file", "memory"} <= names, names)
        return tool_call(
            "write_file",
            {"path": str(child_output), "content": "delegated"},
        )

    def child_reply(_, body):
        succeeded = any(
            message.get("role") == "tool" and "wrote" in str(message.get("content", ""))
            for message in body["messages"]
        )
        return event({"content": "child-route-ok" if succeeded else "child-route-bad"})

    child = Server([child_action, child_reply])

    def delegate(_, body):
        task = function_tool(body, "task")
        assert_true("provider" not in task["parameters"]["properties"], task)
        description = task["parameters"]["properties"]["model"]["description"]
        assert_true("codex-local/MODEL" in description, description)
        runtime = "\n".join(
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "system"
        )
        assert_true("[delegation: parent=" in runtime, runtime)
        return tool_call(
            "task",
            {
                "prompt": "child",
                "mode": "full",
                "model": "codex-local/child model",
            },
        )

    def finish(_, body):
        combined = "\n".join(str(message.get("content", "")) for message in body["messages"])
        return event({"content": "route-ok" if "child-route-ok" in combined else "route-bad"})

    parent = Server([delegate, lambda *_: tool_call("activity_wait", {"wait_ms": 30000}), finish])
    try:
        env = base_env(home, parent.url)
        env["UAGENT_CONTEXT"] = "32768"
        env["UAGENT_PROVIDERS"] = json.dumps(
            {
                "codex-local": {
                    "base_url": child.url,
                    "api_key": "local-key",
                    "protocol": "openrouter",
                    "models": {"child-model": {"id": "child-model", "effort": "low"}},
                }
            }
        )
        result = run_dialog(
            root,
            env,
            "delegate locally\ny\n/q\n",
            timeout=20,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("route-ok" in result.stdout, result.stdout)
        assert_true(
            child_output.read_text(encoding="utf-8") == "delegated",
            child_output.read_text(encoding="utf-8"),
        )
        assert_true(len(child.requests) == 2, len(child.requests))
    finally:
        parent.close()
        child.close()


def test_subagent_can_mix_model_routes(root, home):
    seen = []

    def child(label, expected_model):
        def respond(_, body):
            assert_true(body.get("model") == expected_model, body.get("model"))
            seen.append(label)
            return event({"content": f"{label}-ok"})

        return Server([respond])

    fast = child("fast", "fast-model")
    strong = child("strong", "strong-model")

    def delegate(_, body):
        task = function_tool(body, "task")
        description = task["parameters"]["properties"]["model"]["description"]
        assert_true("fast/child" in description, description)
        assert_true("strong/child" in description, description)
        return event(
            {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "fast-task",
                        "function": {
                            "name": "task",
                            "arguments": json.dumps(
                                {"prompt": "fast child", "model": "fast/child"}
                            ),
                        },
                    },
                    {
                        "index": 1,
                        "id": "strong-task",
                        "function": {
                            "name": "task",
                            "arguments": json.dumps(
                                {"prompt": "strong child", "model": "strong/child"}
                            ),
                        },
                    },
                ]
            },
            finish="tool_calls",
        )

    def finish(_, body):
        content = "\n".join(str(message.get("content", "")) for message in body["messages"])
        valid = "fast-ok" in content and "strong-ok" in content
        return event({"content": "mixed-routes-ok" if valid else "mixed-routes-bad"})

    def wait(*_):
        return tool_call("activity_wait", {"wait_ms": 30000})

    parent = Server([delegate, wait, wait, finish])
    try:
        env = base_env(home, parent.url)
        env["UAGENT_CONTEXT"] = "32768"
        env["UAGENT_PROVIDERS"] = json.dumps(
            {
                "fast": {
                    "base_url": fast.url,
                    "api_key": "fast-key",
                    "models": {"child": "fast-model"},
                },
                "strong": {
                    "base_url": strong.url,
                    "api_key": "strong-key",
                    "models": {"child": "strong-model"},
                },
            }
        )
        result = run(root, env, "--yolo", "-p", "delegate by capability", timeout=20)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "mixed-routes-ok", result.stdout)
        assert_true(sorted(seen) == ["fast", "strong"], seen)
    finally:
        parent.close()
        fast.close()
        strong.close()


def test_subagent_recursion_is_depth_bounded(root, home):
    """A subagent may delegate again, but only while under UAGENT_SUBAGENT_DEPTH —
    the cap is what keeps nested spawning from fanning out without limit."""

    def has_task(body):
        return "task" in function_names(body)

    for depth, cap, expected in (
        ("0", "2", True),
        ("1", "2", True),
        ("2", "2", False),
        ("0", "0", False),
    ):
        server = Server([lambda _, body: event({"content": str(has_task(body))})])
        try:
            env = base_env(home, server.url)
            env["UAGENT_DEPTH"] = depth
            env["UAGENT_SUBAGENT_DEPTH"] = cap
            result = run(root, env, "-p", "probe")
            assert_true(result.returncode == 0, result.stderr)
            assert_true(
                result.stdout.strip() == str(expected),
                (depth, cap, expected, result.stdout),
            )
        finally:
            server.close()


def test_headless_reaps_timed_out_process(root, home):
    workspace = root / "timed-out-process-workspace"
    workspace.mkdir()
    pid_file = workspace / "pid"
    command = (
        f"echo $$ > {shlex.quote(str(pid_file))}; printf 'partial-before-timeout\\n'; sleep 30"
    )
    server = Server([tool_call("run", {"command": command})])
    try:
        trace = workspace / "trace.jsonl"
        env = base_env(home, server.url)
        env["UAGENT_MAX_TURN_SECONDS"] = "1"
        result = run(
            workspace,
            env,
            "--yolo",
            f"--debug={trace}",
            "-p",
            "probe",
            timeout=8,
        )
        assert_true(result.returncode == 1, (result.stdout, result.stderr))
        events = [json.loads(line) for line in trace.read_text().splitlines()]
        assert_true(
            any(
                event["event"] == "tool_result"
                and "partial-before-timeout" in event["data"].get("result", "")
                for event in events
            ),
            events,
        )
        pid = int(pid_file.read_text(encoding="utf-8"))
        time.sleep(0.1)
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError(f"timed-out process {pid} survived uagent exit")
    finally:
        server.close()


def test_detached_terminal_survives_and_is_readable(root, home):
    workspace = root / "detached-terminal-workspace"
    workspace.mkdir()
    pid_file = workspace / "pid"
    command = (
        f"echo $$ > {shlex.quote(str(pid_file))}; "
        "i=0; while [ $i -lt 500 ]; do "
        "printf 'bounded-log-line-%04d-xxxxxxxxxxxxxxxx\\n' \"$i\"; i=$((i+1)); "
        "done; printf 'server-ready\\n'; sleep 30"
    )

    pid = None
    try:
        launch_server = Server(
            [
                tool_call(
                    "run",
                    {"command": command, "detach": True},
                ),
                event({"content": "launched"}),
            ]
        )
        try:
            launch_env = base_env(home, launch_server.url)
            launch_env["UAGENT_BASH_LOG_BYTES"] = "4096"
            launched = run_dialog(
                workspace,
                launch_env,
                "launch the server\n/q\n",
                "--yolo",
                timeout=8,
            )
            for _ in range(40):
                if pid_file.exists():
                    break
                time.sleep(0.05)
            assert_true(pid_file.exists(), "detached command did not start")
            pid = int(pid_file.read_text(encoding="utf-8"))
            assert_true(launched.returncode == 0, launched.stderr)
            assert_true("launched" in launched.stdout, launched.stdout)
            assert_true("bg:1" in launched.stdout, launched.stdout)
            os.kill(pid, 0)
        finally:
            launch_server.close()

        fresh_server = Server([event({"content": "fresh"})])
        try:
            fresh = run_dialog(
                workspace,
                base_env(home, fresh_server.url),
                "status\n/q\n",
                timeout=8,
            )
            assert_true(fresh.returncode == 0, fresh.stderr)
            assert_true("terminals:" not in fresh.stdout, fresh.stdout)
        finally:
            fresh_server.close()

        def request_output(_, body):
            tool_results = [
                message.get("content", "")
                for message in body["messages"]
                if message.get("role") == "tool"
            ]
            listing = next(
                (text for text in tool_results if text.startswith("[running] activity ")),
                "",
            )
            assert_true(f"activity {pid} " in listing, listing)
            return tool_call("activity_output", {"id": pid})

        def verify_output(_, body):
            tool_results = [
                message.get("content", "")
                for message in body["messages"]
                if message.get("role") == "tool"
            ]
            readable = any(
                text.startswith("[running · activity ") and "server-ready" in text
                for text in tool_results
            )
            return event({"content": "terminal-ok" if readable else "terminal-bad"})

        def offer_output(_, body):
            names = function_names(body)
            assert_true("activity_output" in names, names)
            return tool_call("activity_output", {})

        server = Server([offer_output, request_output, verify_output])
        try:
            inspect_env = base_env(home, server.url)
            inspect_env["UAGENT_CONTEXT"] = "16384"
            result = run(
                workspace,
                inspect_env,
                "--yolo",
                "-p",
                "inspect the server from this new session",
                timeout=8,
            )
            assert_true(result.returncode == 0, result.stderr)
            assert_true(result.stdout.strip() == "terminal-ok", result.stdout)
            os.kill(pid, 0)
            record = home / ".uagent" / "terminals" / f"{pid}.json"
            assert_true(record.exists(), record)
            record_data = json.loads(record.read_text(encoding="utf-8"))
            log = pathlib.Path(record_data["log"])
            log_bytes = sum(
                path.stat().st_size
                for path in (log, pathlib.Path(str(log) + ".1"))
                if path.exists()
            )
            assert_true(log_bytes <= 4096, log_bytes)
            assert_true(pathlib.Path(str(log) + ".1").exists(), log)
        finally:
            server.close()

        def verify_stop(_, body):
            result = next(
                message.get("content", "")
                for message in reversed(body["messages"])
                if message.get("role") == "tool"
            )
            return event(
                {"content": "terminal-stop-ok" if "stopped process group" in result else result}
            )

        stop_server = Server([tool_call("activity_stop", {"id": pid}), verify_stop])
        try:
            stop_env = base_env(home, stop_server.url)
            stop_env["UAGENT_CONTEXT"] = "16384"
            stopped = run(
                workspace,
                stop_env,
                "--yolo",
                "-p",
                "stop the detached server",
                timeout=8,
            )
            assert_true(stopped.returncode == 0, stopped.stderr)
            assert_true(stopped.stdout.strip() == "terminal-stop-ok", stopped.stdout)
            assert_true(not record.exists(), record)
            assert_true(not log.exists(), log)
            assert_true(not pathlib.Path(str(log) + ".1").exists(), log)
            try:
                os.killpg(pid, 0)
            except ProcessLookupError:
                pass
            else:
                raise AssertionError(f"detached process group {pid} survived activity_stop")
            pid = None
        finally:
            stop_server.close()
    finally:
        if pid is not None:
            try:
                os.killpg(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass


def test_detached_terminal_preserves_exit_and_waits(root, home):
    state = {"pid": None, "notes": []}

    def wait_for_failure(_, body):
        result = next(
            (
                message.get("content", "")
                for message in reversed(body["messages"])
                if message.get("role") == "tool" and "[detached] pid " in message.get("content", "")
            ),
            "",
        )
        match = re.search(r"\[detached\] pid (\d+)", result)
        assert_true(match is not None, result)
        state["pid"] = int(match.group(1))
        return tool_call("activity_output", {"id": state["pid"], "wait_ms": 2000})

    def completion_note(body):
        state["notes"] = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "system"
        ]
        return any(
            "[Detached result:" in note and "[exit code 7]" in note for note in state["notes"]
        )

    def verify_or_wait(_, body):
        if completion_note(body):
            return event({"content": "detached-exit-ok"})
        return tool_call("activity_output", {"id": state["pid"], "wait_ms": 2000})

    def fail_if_missing(_, body):
        completion_note(body)
        return event({"content": "detached-exit-bad"})

    server = Server(
        [
            tool_call(
                "run",
                {"command": "printf 'detached-failure\\n'; exit 7", "detach": True},
            ),
            wait_for_failure,
            verify_or_wait,
            verify_or_wait,
            verify_or_wait,
            fail_if_missing,
        ]
    )
    try:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "run failure", timeout=12)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(
            result.stdout.strip() == "detached-exit-ok",
            (result.stdout, result.stderr, state["notes"]),
        )
    finally:
        server.close()
        if state["pid"] is not None:
            try:
                os.killpg(state["pid"], signal.SIGTERM)
            except ProcessLookupError:
                pass


def test_detached_terminal_blocking_wait_can_repeat(root, home):
    state = {"pid": None}

    def request_wait(_, body):
        if state["pid"] is None:
            result = next(
                message.get("content", "")
                for message in reversed(body["messages"])
                if message.get("role") == "tool" and "[detached] pid " in message.get("content", "")
            )
            state["pid"] = int(re.search(r"\[detached\] pid (\d+)", result).group(1))
        return tool_call("activity_output", {"id": state["pid"], "wait_ms": 1000})

    server = Server(
        [
            tool_call("run", {"command": "sleep 20", "detach": True}),
            request_wait,
            request_wait,
            request_wait,
            request_wait,
            event({"content": "blocking-wait-ok"}),
        ]
    )
    try:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "wait", timeout=10)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(result.stdout.strip() == "blocking-wait-ok", result.stdout)
        assert_true("repeated the same tool call" not in result.stderr, result.stderr)
    finally:
        server.close()
        if state["pid"] is not None:
            try:
                os.killpg(state["pid"], signal.SIGTERM)
            except ProcessLookupError:
                pass


def test_detached_terminal_waits_for_readiness_marker(root, home):
    state = {"pid": None}

    def wait_for_ready(_, body):
        result = next(
            message.get("content", "")
            for message in reversed(body["messages"])
            if message.get("role") == "tool" and "[detached] pid " in message.get("content", "")
        )
        state["pid"] = int(re.search(r"\[detached\] pid (\d+)", result).group(1))
        return tool_call(
            "activity_output",
            {"id": state["pid"], "wait_ms": 3000, "until": "server-ready"},
        )

    def verify_ready(_, body):
        result = next(
            message.get("content", "")
            for message in reversed(body["messages"])
            if message.get("role") == "tool"
        )
        ready = "server-ready" in result and "wait timed out" not in result
        return event({"content": "readiness-ok" if ready else "readiness-bad"})

    server = Server(
        [
            tool_call(
                "run",
                {
                    "command": "printf 'booting\\n'; sleep 0.4; printf 'server-ready\\n'; sleep 20",
                    "detach": True,
                },
            ),
            wait_for_ready,
            verify_ready,
        ]
    )
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "16384"
        result = run(root, env, "--yolo", "-p", "start server", timeout=10)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(result.stdout.strip() == "readiness-ok", result.stdout)
    finally:
        server.close()
        if state["pid"] is not None:
            try:
                os.killpg(state["pid"], signal.SIGTERM)
            except ProcessLookupError:
                pass


def test_detached_terminal_tracks_group_after_wrapper_exit(root, home):
    state = {"pid": None, "child": None}
    child_file = root / "detached-child-pid"

    def kill_wrapper(_, body):
        result = next(
            message.get("content", "")
            for message in reversed(body["messages"])
            if message.get("role") == "tool" and "[detached] pid " in message.get("content", "")
        )
        state["pid"] = int(re.search(r"\[detached\] pid (\d+)", result).group(1))
        for _ in range(40):
            if child_file.exists():
                break
            time.sleep(0.05)
        assert_true(child_file.exists(), "detached child did not start")
        state["child"] = int(child_file.read_text(encoding="utf-8"))
        return tool_call("run", {"command": f"kill -KILL {state['pid']}"})

    def inspect_group(_, body):
        return tool_call("activity_output", {"id": state["pid"]})

    def stop_group(_, body):
        result = next(
            message.get("content", "")
            for message in reversed(body["messages"])
            if message.get("role") == "tool"
        )
        os.kill(state["child"], 0)
        tracked = result.startswith(f"[detached · activity {state['pid']} ")
        return (
            tool_call("activity_stop", {"id": state["pid"]})
            if tracked
            else event({"content": "group-tracking-bad"})
        )

    def verify_group_stopped(_, body):
        result = next(
            message.get("content", "")
            for message in reversed(body["messages"])
            if message.get("role") == "tool"
        )
        try:
            os.killpg(state["pid"], 0)
        except ProcessLookupError:
            gone = True
        else:
            gone = False
        cleaned = not (home / ".uagent" / "terminals" / f"{state['pid']}.json").exists()
        ok = "stopped process group" in result and gone and cleaned
        return event({"content": "group-tracking-ok" if ok else "group-tracking-bad"})

    server = Server(
        [
            tool_call(
                "run",
                {
                    "command": f"sleep 20 & child=$!; printf '%s\\n' \"$child\" > {shlex.quote(str(child_file))}; wait",
                    "detach": True,
                },
            ),
            kill_wrapper,
            inspect_group,
            stop_group,
            verify_group_stopped,
        ]
    )
    try:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "16384"
        result = run(root, env, "--yolo", "-p", "manage server", timeout=10)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(result.stdout.strip() == "group-tracking-ok", result.stdout)
    finally:
        server.close()
        if state["pid"] is not None:
            try:
                os.killpg(state["pid"], signal.SIGKILL)
            except ProcessLookupError:
                pass


def main():
    requested_group = "all"
    if len(sys.argv) == 4 and sys.argv[2] == "--group":
        requested_group = sys.argv[3]
    elif len(sys.argv) != 2:
        raise SystemExit("usage: integration.py BINARY [--group GROUP]")
    with tempfile.TemporaryDirectory(prefix="uagent-integration-") as temp:
        root = pathlib.Path(temp)
        tests = [
            value
            for name, value in globals().items()
            if name.startswith("test_")
            and callable(value)
            and (requested_group == "all" or integration_group(name) == requested_group)
        ]
        if not tests:
            raise SystemExit(f"unknown or empty integration group: {requested_group}")
        for test in tests:
            print(f"running {test.__name__}", flush=True)
            case_root = root / test.__name__
            home = case_root / "home"
            home.mkdir(parents=True)
            test(case_root, home)
        print(f"all {len(tests)} {requested_group} integration tests passed")


if __name__ == "__main__":
    main()
