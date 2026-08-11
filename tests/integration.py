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


def write_http_response(handler, data, content_type="application/json", status=200):
    handler.send_response(status)
    handler.send_header("Content-Type", content_type)
    handler.send_header("Content-Length", str(len(data)))
    handler.end_headers()
    try:
        handler.wfile.write(data)
    except (BrokenPipeError, ConnectionResetError):
        pass


def write_json_response(handler, payload, status=200):
    write_http_response(handler, json.dumps(payload).encode(), status=status)


def unused_api_url():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return f"http://127.0.0.1:{sock.getsockname()[1]}/v1"


def two_route_providers(first_url, second_url):
    return {
        "first": {
            "base_url": first_url,
            "api_key": "key-a",
            "context": 4096,
            "models": {"main": {"id": "model-a", "effort": "low"}},
        },
        "second": {
            "base_url": second_url,
            "api_key": "key-b",
            "models": {"fast": {"id": "model-b", "effort": "medium"}},
        },
    }


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
                write_json_response(self, get_response)

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
                write_http_response(
                    self,
                    data,
                    "text/event-stream" if streaming else "application/json",
                )

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
            "UAGENT_CONTEXT": "16384",
            "UAGENT_REQUEST_TIMEOUT": "5",
            "UAGENT_FIRST_EVENT_TIMEOUT": "2",
            "UAGENT_STREAM_IDLE_TIMEOUT": "2",
            "UAGENT_CHROME_DEVTOOLS": "0",
        }
    )
    return env


def write_route_profile(home, url, **features):
    path = home / ".uagent" / "config" / "routes.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    key = f"{urllib.parse.urlparse(url).netloc}||test|"
    profile = {
        "samples": 4,
        "passing_samples": 4,
        "pass_rate": 1.0,
        "certified": True,
        "scenario_classes": ["analysis"],
        "certified_at_unix": int(time.time()),
    }
    profile.update(features)
    path.write_text(
        json.dumps({"schema": 3, "routes": {key: profile}, "recommendations": {}}),
        encoding="utf-8",
    )
    return path, key


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
                    chunk = os.read(master, 65536)
                except OSError:
                    return False
                if not chunk:
                    return False
                output.extend(chunk)
            elif process.poll() is not None:
                return False
        return marker is None

    def read_prompt(start=0):
        read_until(b"\x1b[36m> \x1b[0m\x1b[39m\x1b[49m", start)
        time.sleep(0.05)  # the composer finishes raw-mode setup after drawing

    def write_fragment(fragment):
        offset = 0
        while offset < len(fragment):
            readable, writable, _ = select.select([master], [master], [], 0.1)
            if readable:
                chunk = os.read(master, 65536)
                if not chunk:
                    return
                output.extend(chunk)
            if process.poll() is not None:
                return
            if writable:
                offset += os.write(master, fragment[offset : offset + 4096])

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
                fragments = item if isinstance(item, list) else [item]
                for fragment in fragments:
                    write_fragment(fragment)
                    if len(fragments) > 1:
                        time.sleep(0.01)
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


def timeout_retry_env(home, url):
    env = base_env(home, url)
    env.update(
        {
            "UAGENT_REQUEST_TIMEOUT": "1",
            "UAGENT_FIRST_EVENT_TIMEOUT": "0",
            "UAGENT_STREAM_IDLE_TIMEOUT": "0",
            "UAGENT_MAX_TURN_SECONDS": "6",
        }
    )
    return env


def tool_calls(calls, usage=None):
    return event(
        {
            "tool_calls": [
                {
                    "index": index,
                    "id": call_id,
                    "function": {"name": name, "arguments": json.dumps(arguments)},
                }
                for index, (call_id, name, arguments) in enumerate(calls)
            ]
        },
        finish="tool_calls",
        usage=usage,
    )


def tool_call(name, arguments, *, call_id="call-1", usage=None):
    return tool_calls([(call_id, name, arguments)], usage)


def read_file_calls(paths):
    return tool_calls(
        [(f"call-{index}", "read_file", {"path": str(path)}) for index, path in enumerate(paths)]
    )


def handoff_env(home, first_url, second_url):
    env = base_env(home, first_url)
    env["UAGENT_PROVIDERS"] = json.dumps(
        {
            "cheap": {
                "base_url": first_url,
                "api_key": "cheap-key",
                "models": {"flash": "flash-model"},
            },
            "strong": {
                "base_url": second_url,
                "api_key": "strong-key",
                "models": {"main": "strong-model"},
            },
        }
    )
    env["UAGENT_MODEL"] = "cheap/flash"
    return env


def run_queued_interrupt(workspace, home, url, initial):
    code, output = run_pty(
        workspace,
        base_env(home, url),
        [
            initial,
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
    return output


def detached_pid(body):
    result = next(
        message.get("content", "")
        for message in reversed(body["messages"])
        if message.get("role") == "tool" and "[detached] pid " in message.get("content", "")
    )
    match = re.search(r"\[detached\] pid (\d+)", result)
    assert_true(match is not None, result)
    return int(match.group(1))


def signal_process_group(pid, signal_number=signal.SIGTERM):
    if pid is None:
        return
    try:
        os.killpg(pid, signal_number)
    except ProcessLookupError:
        pass


def message_with_prefix(messages, prefix):
    return next(
        (
            message
            for message in messages
            if isinstance(message.get("content"), str) and message["content"].startswith(prefix)
        ),
        {},
    )


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


def checkpoint_env(home, url, mode):
    env = base_env(home, url)
    env["UAGENT_CHECKPOINT_MODE"] = mode
    env["UAGENT_CHECKPOINT_PCT"] = "1"
    env["UAGENT_CHECKPOINT_URGENT_PCT"] = "2"
    env["UAGENT_AUTO_COMPACT_PCT"] = "0"
    return env


def midturn_compaction_env(home, url):
    env = base_env(home, url)
    env.update(
        {
            "UAGENT_CONTEXT": "8000",
            "UAGENT_MAX_TOKENS": "512",
            "UAGENT_AUTO_COMPACT_PCT": "40",
            "UAGENT_CHECKPOINT_MODE": "off",
            "UAGENT_TOOL_RESULT_CHARS": "8000",
        }
    )
    return env


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
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "4096"
        trace = workspace / "trace.jsonl"
        result = run(
            workspace,
            env,
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
            "run_python(write/replace trace.py → execute)",
            "[script: .uagent/scratch/trace.py · wrote · executed]",
            "python-one",
            "python-two",
            "trace-ok",
        ):
            assert_true(expected in result.stdout, result.stdout)
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
        paste = [
            b"\x1b",
            b"[2",
            b"00~first line\r\nsecond line\rthird line\x1b[20",
            b"1~",
        ]
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


def test_input_redraw_focus_switch_preserves_multiline_draft(root, home):
    def verify(_, body):
        pasted = body["messages"][-1].get("content")
        return event(
            {
                "content": (
                    "focus-draft-ok"
                    if pasted == "first line\nsecond line\nthird line"
                    else "focus-draft-bad"
                )
            }
        )

    server = Server([verify])
    try:
        input_fragments = [
            b"\x1b[200~first line\r\nsecond line\x1b[201~",
            b"\x1b",
            b"[",
            b"O",
            b"\x1b[",
            b"I",
            b"\x1b[200~\nthird line\x1b[201~",
        ]
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(input_fragments, b"third"), b"\n", b"\x04"],
            columns=24,
        )
        assert_true(code == 0, output)
        assert_true(b"focus-draft-ok" in output, output)
        assert_true(b"[Ifirst" not in output and b"[Ofirst" not in output, output)
        assert_true(b"response interrupted" not in output, output)
        assert_true(len(server.requests) == 1, len(server.requests))
    finally:
        server.close()


def test_input_redraw_bare_escape_still_clears_idle_draft(root, home):
    def verify(_, body):
        user = body["messages"][-1].get("content")
        return event({"content": "bare-escape-ok" if user == "kept" else "bare-escape-bad"})

    server = Server([verify])
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"discard me", b"discard"),
                ([b"\x1b"] + [b""] * 7, b"> "),
                b"kept\n",
                b"\x04",
            ],
        )
        assert_true(code == 0, output)
        assert_true(b"bare-escape-ok" in output, output)
        assert_true(len(server.requests) == 1, len(server.requests))
    finally:
        server.close()


def test_input_redraw_history_restores_current_draft(root, home):
    def verify_draft(_, body):
        user = body["messages"][-1].get("content")
        return event({"content": "history-draft-ok" if user == "draft" else "history-draft-bad"})

    server = Server([event({"content": "first-ok"}), verify_draft])
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"first\n", b"first-ok"),
                (b"draft", b"draft"),
                (b"\x1b[A", b"first"),
                b"\x1b[B\n",
                b"\x04",
            ],
        )
        assert_true(code == 0, output)
        assert_true(b"history-draft-ok" in output, output)
        assert_true(len(server.requests) == 2, server.requests)
    finally:
        server.close()


def test_input_redraw_approval_does_not_pollute_history(root, home):
    def verify_recalled(_, body):
        user = body["messages"][-1].get("content")
        return event({"content": "approval-history-ok" if user == "go" else "approval-history-bad"})

    server = Server(
        [
            tool_call("run", {"command": "printf approved"}),
            event({"content": "approval-done"}),
            verify_recalled,
        ]
    )
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"go\n", b"allow run?"),
                (b"y\n", b"approval-done"),
                (b"\x1b[A\n", b"approval-history-ok"),
                b"\x04",
            ],
        )
        assert_true(code == 0, output)
        assert_true(b"approval-history-ok" in output, output)
        assert_true(len(server.requests) == 3, server.requests)
    finally:
        server.close()


def test_input_redraw_enter_then_escape_same_packet_interrupts_turn(root, home):
    def delayed(_, __):
        time.sleep(2)
        return event({"content": "too-late"})

    server = Server([delayed])
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"work\n\x1b", b"interrupting"), b"/q\n"],
            timeout=8,
        )
        assert_true(code == 0, output)
        assert_true(b"interrupting" in output, output)
        assert_true(b"too-late" not in output, output)
    finally:
        server.close()


def test_input_redraw_status_animation_does_not_repaint_draft(root, home):
    def delayed(_, __):
        time.sleep(0.7)
        return event({"content": "status-redraw-ok"})

    server = Server([delayed])
    try:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"work\n", b"working"),
                (b"pending draft", b"status-redraw-ok"),
                b"\x15/q\n",
            ],
        )
        assert_true(code == 0, output)
        assert_true(b"status-redraw-ok" in output, output)
        assert_true(output.count(b"pending draft") <= 2, output)
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
    providers = two_route_providers(first.url, second.url)
    providers["second"]["models"]["fast"]["context"] = 8192
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
    try:
        env = handoff_env(home, first.url, second.url)
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


def test_model_preference_survives_restart(root, home):
    first = Server([event({"content": "explicit-model-ok"})])

    def remembered(_, body):
        valid = body.get("model") == "model-b" and body.get("reasoning_effort") == "medium"
        return event({"content": "remembered-model-ok" if valid else "remembered-model-bad"})

    second = Server([remembered])
    providers = two_route_providers(first.url, second.url)
    providers["second"]["context"] = 8192
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


def test_checkpoint_apply(root, home):
    workspace = root / "checkpoint-apply"
    workspace.mkdir()
    (workspace / "state.txt").write_text("durable file state\n", encoding="utf-8")

    def folded(_, body):
        messages = body["messages"]
        checkpoint = message_with_prefix(messages, "[checkpoint facts; non-authoritative]")
        retained_file = message_with_prefix(messages, "[checkpoint file state.txt;")
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


def test_midturn_compaction_preserves_progress_and_usage(root, home):
    trace = root / "midturn-compact.jsonl"
    source = root / "midturn-source.txt"
    source.write_text("RAW-TOOL-RESULT-" + "x" * 7800, encoding="utf-8")
    output = root / "midturn-output.txt"

    first = tool_call(
        "read_file",
        {"path": str(source)},
        call_id="midturn-call",
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
        env = midturn_compaction_env(home, server.url)
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
                "launch the server\n/ps\n/q\n",
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
            assert_true("background work" in launched.stdout, launched.stdout)
            assert_true("[detached] activity" in launched.stdout, launched.stdout)
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
        signal_process_group(pid)


def test_detached_terminal_tracks_group_after_wrapper_exit(root, home):
    state = {"pid": None, "child": None}
    child_file = root / "detached-child-pid"

    def kill_wrapper(_, body):
        state["pid"] = detached_pid(body)
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
        result = run(root, env, "--yolo", "-p", "manage server", timeout=10)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(result.stdout.strip() == "group-tracking-ok", result.stdout)
    finally:
        server.close()
        signal_process_group(state["pid"], signal.SIGKILL)


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
