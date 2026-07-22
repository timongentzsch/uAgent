#!/usr/bin/env python3
import json
import os
import pathlib
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

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
                self.wfile.write(data)

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


def test_response_stats(root, home):
    server = Server([event({"content": "stats-ok"}, usage={"completion_tokens": 4})])
    try:
        result = run_dialog(root, base_env(home, server.url), "hello\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true("tok/s" in result.stdout, result.stdout)
        assert_true("ttt " in result.stdout, result.stdout)
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
        assert_true("view_image" not in names, names)
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
            "aW1hZ2U=",
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
                if all(value in result for value in expected)
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
        assert_true("chrome-devtools_list_pages" in names, names)
        assert_true("chrome_session" in names, names)
        return tool_call("chrome_session", {"mode": "user"})

    def final(_, body):
        result = next(
            message["content"] for message in body["messages"] if message.get("role") == "tool"
        )
        expected = "approval prompt appears only when the next browser tool interacts"
        return event({"content": "chrome-ok" if expected in result else "chrome-bad"})

    server = Server([switch, final])
    try:
        env = base_env(home, server.url)
        env["UAGENT_CHROME_DEVTOOLS"] = "1"
        env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
        result = run(workspace, env, "--yolo", "-p", "use my browser")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "chrome-ok", result.stdout)
        calls = invocations.read_text(encoding="utf-8").splitlines()
        assert_true(len(calls) == 2, calls)
        assert_true("--isolated" in calls[0], calls)
        assert_true("chrome-devtools-mcp@latest" in calls[0], calls)
        assert_true("--auto-connect" in calls[1] and "--isolated" not in calls[1], calls)
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
            tool_call("run_bash", {"command": command, "timeout": 1}),
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

    server = Server([tool_call("run_bash", {"command": "printf should-not-run"}), final])
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
        messages = body["messages"]
        valid = messages[-1].get("content") == "continue" and messages[-2].get("role") == "tool"
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


def test_headless_reaps_background_process(root, home):
    workspace = root / "background-workspace"
    workspace.mkdir()
    pid_file = workspace / "pid"
    command = f"echo $$ > {shlex.quote(str(pid_file))}; sleep 30"
    server = Server(
        [
            tool_call("run_bash", {"command": command, "timeout": 1}),
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


def main():
    with tempfile.TemporaryDirectory(prefix="uagent-integration-") as temp:
        root = pathlib.Path(temp)
        home = root / "home"
        home.mkdir()
        tests = [
            test_plain_turn,
            test_response_stats,
            test_headless_debug_session_end,
            test_grep_tool_round_trip,
            test_real_headless_error,
            test_project_mcp_trust,
            test_invalid_mcp_config_not_executed,
            test_mcp_tool_round_trip,
            test_mcp_stdio_contract,
            test_builtin_chrome_session_modes,
            test_mcp_tool_list_changed,
            test_user_config_interpolation,
            test_model_route_switch,
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
            test_headless_reaps_background_process,
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
