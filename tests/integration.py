#!/usr/bin/env python3
import base64
import fcntl
import json
import os
import pathlib
import pty
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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Enough of a PNG for the attachment inspector to accept it.
SMALL_PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 32
BINARY = pathlib.Path(sys.argv[1]).resolve()


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
                data = sse(response)
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                try:
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_):
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
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
        read_until(b"\x1b[?2004h", start, following=b">")
        time.sleep(0.1)  # libedit finishes terminal setup after drawing the prompt

    read_prompt()
    if interrupt:
        process.send_signal(signal.SIGINT)
    else:
        payloads = [payload] if isinstance(payload, bytes) else payload
        for index, item in enumerate(payloads):
            marker = None
            if isinstance(item, tuple):
                item, marker = item
            start = len(output)
            os.write(master, item)
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


def test_plain_turn(root, home):
    server = Server([event({"content": "ok"}, usage={"prompt_tokens": 2, "completion_tokens": 1})])
    try:
        result = run(root, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "ok", result.stdout)
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
            and messages[1].get("role") == "user"
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
    pdf.write_bytes(b"%PDF-1.4\n" + b"\x00" * 32)
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
        result = run(workspace, base_env(home, server.url), "--yolo", "-p", "look", timeout=40)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "attach-ok", result.stdout)
        assert_true(seen.get("image"), seen)
        assert_true(seen.get("file"), seen)
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
            tool_call("run_python", {"code": python_code}),
            event({"content": "trace-ok"}),
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
        for expected in (
            "printf 'shell-one",
            "printf 'shell-two",
            "shell-one",
            "shell-two",
            "print('python-one')",
            "print('python-two')",
            "python-one",
            "python-two",
            "trace-ok",
        ):
            assert_true(expected in result.stdout, result.stdout)
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
            ("Apple_Terminal", "xterm-256color", "Images unavailable", False, empty_path),
            ("ghostty", "xterm-ghostty", "show_image (native)", True, empty_path),
            (
                "Apple_Terminal",
                "xterm-256color",
                "Images unavailable",
                False,
                chafa_path,
            ),
        )
        for program, term, instruction, has_tool, path in cases:

            def verify(_, body, instruction=instruction, has_tool=has_tool):
                names = {tool["function"]["name"] for tool in body["tools"]}
                has_instruction = instruction in body["messages"][0]["content"]
                has_image_tool = "show_image" in names
                valid = has_instruction and has_image_tool == has_tool
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
            [(paste, b"third line"), b"\n", b"\x04"],
            columns=24,
        )
        assert_true(code == 0, output)
        assert_true(b"multiline-paste-ok" in output, output)
        assert_true(b"/4.1K (" in output, output)
        assert_true(b"\x1b[?2004h" in output and b"\x1b[?2004l" in output, output)
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
    server = Server([event({"content": "stats-ok"}, usage={"completion_tokens": 4})])
    try:
        result = run_dialog(root, base_env(home, server.url), "hello\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true("tok/s" in result.stdout, result.stdout)
        assert_true("first " in result.stdout, result.stdout)
    finally:
        server.close()


def test_cacheable_prefix_stable_across_turns(root, home):
    """The clock must ride on the turn, not messages[0]: rewriting the system
    message each turn would invalidate the provider's cached prefix."""
    server = Server([event({"content": "one"}), event({"content": "two"})])
    try:
        result = run_dialog(root, base_env(home, server.url), "first\nsecond\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(len(server.requests) == 2, len(server.requests))
        first, second = (body["messages"] for _, body in server.requests)
        assert_true(first[0] == second[0], (first[0], second[0]))
        assert_true("[now " not in first[0]["content"], first[0]["content"])
        # ...and the model still gets the time, appended per turn
        stamps = [m for m in second if str(m.get("content", "")).startswith("[now ")]
        assert_true(len(stamps) == 2, stamps)
        # every message the first request sent is still a byte-identical prefix
        assert_true(second[: len(first)] == first, (first, second[: len(first)]))
    finally:
        server.close()


def test_wait_background_spinner(root, home):
    workspace = root / "wait-spinner-workspace"
    workspace.mkdir()

    def wait_for_pid(_, body):
        result = body["messages"][-1]["content"]
        pid = int(result.split("[backgrounded] pid ", 1)[1].split(",", 1)[0])
        return tool_call("wait_background", {"id": pid})

    server = Server(
        [
            tool_call(
                "run",
                {"command": "sleep 2; printf 'background-done\\n'", "timeout": 1},
            ),
            wait_for_pid,
            event({"content": "spinner-ok"}),
        ]
    )
    try:
        code, output = run_pty(
            workspace,
            base_env(home, server.url),
            [b"probe\n", b"/q\n"],
            timeout=10,
            args=("--yolo",),
        )
        start = output.find(b"wait_background(job ")
        end = output.find(b"\xe2\x86\x90 wait_background:", start)
        assert_true(code == 0, output)
        assert_true(start >= 0 and end > start, output)
        assert_true(b"\r\x1b[2m" in output[start:end], output[start:end])
        assert_true(b"background-done" in output and b"spinner-ok" in output, output)
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
            "one.cpp" in result and "ignored.txt" not in result and "project_wide_symbol" in result
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
        names = {tool["function"]["name"] for tool in server.requests[0][1].get("tools", [])}
        assert_true("grep" in names, names)
        assert_true("show_image" not in names, names)
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
    (workspace / ".uagent" / "memory").mkdir(parents=True)
    (workspace / ".uagent" / "memory" / "build.md").write_text(
        "project-memory-sentinel", encoding="utf-8"
    )
    (home / ".uagent" / "memory").mkdir(parents=True, exist_ok=True)
    (home / ".uagent" / "memory" / "style.md").write_text(
        "global-memory-sentinel", encoding="utf-8"
    )
    other = root / "memory-other-workspace"
    other.mkdir()

    def verify(_, body):
        instructions = str(body["messages"][1].get("content", ""))
        valid = (
            "## memory: style" in instructions
            and "global-memory-sentinel" in instructions
            and "## memory: build" in instructions
            and "project-memory-sentinel" in instructions
        )
        return event({"content": "memory-ok" if valid else "memory-bad"})

    def verify_isolated(_, body):
        messages = body["messages"]
        instructions = str(messages[1].get("content", "")) if len(messages) > 1 else ""
        valid = (
            "global-memory-sentinel" in instructions
            and "project-memory-sentinel" not in instructions
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
        (home / ".uagent" / "memory" / "style.md").unlink(missing_ok=True)


def test_skill_tool_offers_and_opens(root, home):
    workspace = root / "skill-workspace"
    skill = workspace / ".uagent" / "skills" / "demo"
    skill.mkdir(parents=True)
    (skill / "SKILL.md").write_text(
        "---\nname: demo\ndescription: demo-description-sentinel\n---\n\ndemo-body-sentinel\n",
        encoding="utf-8",
    )

    def offer(_, body):
        functions = {t["function"]["name"]: t["function"] for t in body.get("tools", [])}
        name_arg = functions.get("skill", {}).get("parameters", {}).get("properties", {})
        valid = (
            "skill" in functions
            and name_arg.get("name", {}).get("enum") == ["demo"]
            and "demo-description-sentinel" in name_arg["name"]["description"]
            # progressive disclosure: the catalogue is present, the body is not
            and "demo-body-sentinel" not in json.dumps(body)
        )
        if not valid:
            return event({"content": "schema-bad"})
        return tool_call("skill", {"name": "demo"})

    def confirm(_, body):
        opened = any("demo-body-sentinel" in str(m.get("content", "")) for m in body["messages"])
        return event({"content": "skill-ok" if opened else "skill-bad"})

    server = Server([offer, confirm])
    try:
        code, output = run_pty(
            workspace, base_env(home, server.url), [b"reply\n", b"\x04"], columns=24
        )
        assert_true(code == 0, output)
        assert_true(b"skill-ok" in output and b"tokens/request" in output, output)
        assert_true(b"demo" in output, output)
        assert_true(b"demo-body-sentinel" in output, output)
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
        blob = json.dumps(body["messages"])
        if '"image_url"' in blob or "withheld" not in blob:
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
        result = run(workspace, base_env(home, server.url), "--yolo", "-p", "look")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "degrade-ok", result.stdout + result.stderr)
        # The notice rides on stdout like the other degradations, which headless
        # sends to /dev/null; the retry itself is what this test pins down.
        assert_true(len(server.requests) == 4, len(server.requests))
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
        names = [tool["function"]["name"] for tool in server.requests[0][1].get("tools", [])]
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
        functions = {
            tool["function"]["name"]: tool["function"]
            for tool in server.requests[0][1].get("tools", [])
        }
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
    fake = workspace / "fake_chrome_mcp.py"
    fake.write_text(
        "import json, sys\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    method = message.get('method')\n"
        "    if method == 'initialize':\n"
        "        result = {'protocolVersion': '2025-11-25', "
        "'capabilities': {'tools': {}}, 'serverInfo': {'name': 'chrome', 'version': '1'}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'list_pages', 'description': 'list pages', "
        "'inputSchema': {'type': 'object', 'additionalProperties': False}}]}\n"
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
        f"exec {shlex.quote(sys.executable)} {shlex.quote(str(fake))}\n",
        encoding="utf-8",
    )
    npx.chmod(0o700)

    def switch(_, body):
        names = {tool["function"]["name"] for tool in body.get("tools", [])}
        assert_true("chrome-devtools_list_pages" not in names, names)
        assert_true("chrome_session" in names, names)
        return tool_call("chrome_session", {"mode": "user"})

    def use_browser(_, body):
        names = {tool["function"]["name"] for tool in body.get("tools", [])}
        assert_true("chrome-devtools_list_pages" in names, names)
        result = next(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        expected = "approval prompt appears only when the next browser tool interacts"
        assert_true(expected in result, result)
        return tool_call("chrome-devtools_list_pages", {})

    def final(_, body):
        results = [
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        ]
        return event({"content": "chrome-ok" if results[-1] == "ok" else "chrome-bad"})

    server = Server([switch, use_browser, final])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CHROME_DEVTOOLS"] = "1"
        env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
        result = run(workspace, env, "--yolo", "-p", "use my browser")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "chrome-ok", result.stdout)
        calls = invocations.read_text(encoding="utf-8").splitlines()
        assert_true(len(calls) == 1, calls)
        assert_true("chrome-devtools-mcp@latest" in calls[0], calls)
        assert_true("--auto-connect" in calls[0] and "--isolated" not in calls[0], calls)
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
        names = {tool["function"]["name"] for tool in body.get("tools", [])}
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
            "/models\n/model second/fast\n/effort default\n/effort high\nprobe\n/q\n",
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
            "/models all\n/models alpha\n/q\n",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("vendor/alpha" in result.stdout, result.stdout)
        assert_true("vendor/beta" in result.stdout, result.stdout)
        assert_true("effort low,high (default low)" in result.stdout, result.stdout)
        assert_true("· 1 model" in result.stdout, result.stdout)
        assert_true(server.get_requests == ["/v1/models", "/v1/models"], server.get_requests)
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
            checkpoint.get("role") == "assistant"
            and "Objective remains stable; tests passed; no unresolved conditions."
            in checkpoint.get("content", "")
            and checkpoint.get("role") != "user"
            and retained_file.get("role") == "assistant"
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
        names = {tool["function"]["name"] for tool in server.requests[0][1].get("tools", [])}
        assert_true("checkpoint" in names, names)
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
            checkpoint.get("role") == "assistant"
            and "Objective: validate a 500k context fold." in checkpoint.get("content", "")
            and any(
                message.get("role") == "assistant"
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


def test_checkpoint_invalidated_by_background_job(root, home):
    workspace = root / "checkpoint-invalidated"
    workspace.mkdir()
    command = "sleep 30"

    def not_folded(_, body):
        messages = body["messages"]
        valid = (
            any(
                message.get("role") == "user" and message.get("content") == "first request"
                for message in messages
            )
            and messages[-1].get("role") == "user"
            and messages[-1].get("content") == "third request"
            and not any(
                isinstance(message.get("content"), str)
                and message["content"].startswith("[checkpoint facts; non-authoritative]")
                for message in messages
            )
        )
        return event(
            {"content": "checkpoint-invalidated-ok" if valid else "checkpoint-invalidated-bad"}
        )

    server = Server(
        [
            tool_call("run", {"command": command, "timeout": 1}),
            event({"content": "background-started"}),
            tool_call(
                "checkpoint",
                {
                    "state": "A background job is still unresolved.",
                    "keep_last_n_results": 0,
                },
            ),
            not_folded,
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
        assert_true("checkpoint-invalidated-ok" in result.stdout, result.stdout)
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
            activity.get("role") == "assistant"
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
            checkpoint.get("role") == "assistant"
            and "beta is current" in checkpoint.get("content", "")
            and "alpha is obsolete" in checkpoint.get("content", "")
            and any(
                message.get("role") == "assistant"
                and '["beta","alpha"]' in message.get("content", "")
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
            and checkpoints[0].get("role") == "assistant"
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
            and any(
                message.get("role") == "tool"
                and "malformed tool arguments" in message.get("content", "")
                for message in messages
            )
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

    server = Server([stall])
    try:
        env = base_env(home, server.url)
        env["UAGENT_FIRST_EVENT_TIMEOUT"] = "1"
        started = time.monotonic()
        result = run(root, env, "-p", "probe")
        elapsed = time.monotonic() - started
        assert_true(result.returncode == 1, result.returncode)
        assert_true("no event within 1s" in result.stderr, result.stderr)
        assert_true(elapsed < 1.8, elapsed)
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
        notes = ("[now ", "[context checkpoint")
        real = [m for m in body["messages"] if not str(m.get("content", "")).startswith(notes)]
        index = next((i for i, m in enumerate(real) if m.get("content") == "continue"), 0)
        valid = index > 0 and real[index - 1].get("role") == "tool"
        return event({"content": "history-ok" if valid else "history-bad"})

    server = Server([repeated, repeated, repeated, repeated, after_abort])
    try:
        result = run_dialog(
            root,
            base_env(home, server.url),
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
        assert_true(len(server.requests) == 8, len(server.requests))
    finally:
        server.close()


def test_late_subagent_continues_turn(root, home):
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
        if any(message.get("tool_calls") for message in messages):
            return event({"content": "provisional"})
        return tool_call("task", {"prompt": "child", "timeout": 1})

    server = Server([route])
    try:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "delegate", timeout=8)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "late-task-ok", result.stdout)
        assert_true(len(server.requests) == 4, len(server.requests))
    finally:
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
                        "arguments": json.dumps(
                            {"command": f"sleep {sleep}; echo done{i}", "timeout": 20}
                        ),
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


def test_subagent_timeout_is_capped(root, home):
    """A model-supplied `timeout` must not stretch delegation's foreground window —
    that is what serialises a fleet of subagents instead of overlapping them."""

    def route(_, body):
        messages = body["messages"]
        if any(m.get("role") == "user" and m.get("content") == "child" for m in messages):
            time.sleep(8)  # far longer than the 3s cap
            return event({"content": "child-done"})
        if any(m.get("tool_calls") for m in messages):
            return event({"content": "capped-ok"})
        return tool_call("task", {"prompt": "child", "timeout": 120})

    server = Server([route])
    try:
        started = time.time()
        result = run(root, base_env(home, server.url), "--yolo", "-p", "delegate", timeout=60)
        elapsed = time.time() - started
        assert_true(result.returncode == 0, result.stderr)
        # the spawn must have backgrounded near the 3s cap, not waited out 8s
        assert_true(elapsed < 8, f"delegation blocked {elapsed:.1f}s; cap not applied")
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
            child_requests.append(1)
            # the child would loop forever if its step budget were not enforced
            return tool_call("list_dir", {"path": "."})
        if any(m.get("tool_calls") for m in messages):
            return event({"content": "budget-ok"})
        return tool_call("task", {"prompt": "child", "timeout": 120})

    server = Server([route])
    try:
        env = base_env(home, server.url)
        env["UAGENT_SUBAGENT_MAX_STEPS"] = "1"
        result = run(root, env, "--yolo", "-p", "delegate", timeout=60)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(len(child_requests) == 1, f"child made {len(child_requests)} requests")
    finally:
        server.close()


def test_subagent_recursion_is_depth_bounded(root, home):
    """A subagent may delegate again, but only while under UAGENT_SUBAGENT_DEPTH —
    the cap is what keeps nested spawning from fanning out without limit."""

    def has_task(body):
        return "task" in {tool["function"]["name"] for tool in body.get("tools", [])}

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


def test_headless_reaps_background_process(root, home):
    workspace = root / "background-workspace"
    workspace.mkdir()
    pid_file = workspace / "pid"
    command = f"echo $$ > {shlex.quote(str(pid_file))}; sleep 30"
    server = Server(
        [
            tool_call("run", {"command": command, "timeout": 1}),
            event({"content": "done"}),
        ]
    )
    try:
        result = run(
            workspace,
            base_env(home, server.url),
            "--yolo",
            "-p",
            "probe",
            timeout=8,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "done", result.stdout)
        pid = int(pid_file.read_text(encoding="utf-8"))
        time.sleep(0.1)
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError(f"background process {pid} survived uagent exit")
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
                    {"command": command, "timeout": 0, "detach": True},
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
            assert_true("terminals:1" in launched.stdout, launched.stdout)
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
                (text for text in tool_results if text.startswith("[running] pid ")),
                "",
            )
            assert_true(f"pid {pid} " in listing, listing)
            return tool_call("terminal_output", {"pid": pid})

        def verify_output(_, body):
            tool_results = [
                message.get("content", "")
                for message in body["messages"]
                if message.get("role") == "tool"
            ]
            readable = any(
                text.startswith("[running · pid ") and "server-ready" in text
                for text in tool_results
            )
            return event({"content": "terminal-ok" if readable else "terminal-bad"})

        server = Server(
            [
                tool_call("terminal_output", {}),
                request_output,
                verify_output,
            ]
        )
        try:
            result = run(
                workspace,
                base_env(home, server.url),
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
    finally:
        if pid is not None:
            try:
                os.killpg(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass


def main():
    with tempfile.TemporaryDirectory(prefix="uagent-integration-") as temp:
        root = pathlib.Path(temp)
        home = root / "home"
        home.mkdir()
        tests = [
            test_plain_turn,
            test_project_instructions_precede_first_turn,
            test_attach_tool_puts_bytes_in_context,
            test_attach_flag_headless,
            test_full_run_and_python_terminal_trace,
            test_terminal_image_capability_contract,
            test_multiline_bracketed_paste,
            test_signal_exit_restores_terminal,
            test_response_stats,
            test_cacheable_prefix_stable_across_turns,
            test_wait_background_spinner,
            test_headless_debug_session_end,
            test_grep_tool_round_trip,
            test_real_headless_error,
            test_project_mcp_trust,
            test_project_agent_config_trust,
            test_memory_reaches_context_by_scope,
            test_skill_tool_offers_and_opens,
            test_mcp_image_reaches_the_model,
            test_image_input_rejection_degrades,
            test_invalid_mcp_config_not_executed,
            test_mcp_tool_round_trip,
            test_mcp_stdio_contract,
            test_builtin_chrome_session_modes,
            test_mcp_tool_list_changed,
            test_user_config_interpolation,
            test_model_route_switch,
            test_model_preference_survives_restart,
            test_live_model_catalog,
            test_checkpoint_apply,
            test_checkpoint_500k_window,
            test_checkpoint_shadow,
            test_checkpoint_invalidated_by_background_job,
            test_checkpoint_retains_runtime_activity,
            test_checkpoint_preserves_correction,
            test_checkpoint_multiple_folds,
            test_malformed_checkpoint_ends_apply_turn,
            test_checkpoint_rejects_secret_path,
            test_project_env_ignored,
            test_external_read_requires_approval,
            test_first_event_timeout,
            test_response_size_limit,
            test_turn_cost_limit,
            test_repeated_tool_guard,
            test_repeated_tool_guard_keeps_history_valid,
            test_interleaved_tool_calls_reset_guard,
            test_late_subagent_continues_turn,
            test_parallel_run_overlaps,
            test_subagent_timeout_is_capped,
            test_subagent_receives_budget,
            test_subagent_recursion_is_depth_bounded,
            test_headless_reaps_background_process,
            test_detached_terminal_survives_and_is_readable,
        ]
        names = [test.__name__ for test in tests]
        defined = {
            name
            for name, value in globals().items()
            if name.startswith("test_") and callable(value)
        }
        assert_true(len(names) == len(set(names)), "duplicate integration test registration")
        assert_true(
            set(names) == defined, f"unregistered integration tests: {sorted(defined - set(names))}"
        )
        for test in tests:
            test(root, home)
            print(f"ok {test.__name__}")


if __name__ == "__main__":
    main()
