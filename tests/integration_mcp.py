import base64

from integration_support import (
    SMALL_PNG,
    Server,
    assert_true,
    base_env,
    budget,
    event,
    function_names,
    json,
    run,
    sys,
    time,
    tool_call,
    tool_results,
    write_mcp_server,
)


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
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'shot', 'description': 'screenshot', "
        "'inputSchema': {'type': 'object', 'properties': {}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        meta = message.get('params', {}).get('_meta', {})\n"
        "        assert meta.get('io.modelcontextprotocol/protocolVersion') == '2026-07-28'\n"
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
        tool_text = "".join(tool_results(body["messages"]))
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

    with Server([tool_call("cam_shot", {}), verify]) as server:
        env = base_env(home, server.url)
        result = run(workspace, env, "--trust-project-config", "--yolo", "-p", "screenshot")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "image-ok", result.stdout + result.stderr)


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
    with Server([event({"content": "ok"})]) as server:
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


def test_legacy_mcp_server_is_rejected_without_initialize_fallback(root, home):
    workspace = root / "mcp-legacy-rejected"
    workspace.mkdir()
    marker = workspace / "initialize-called"
    fake = workspace / "fake_mcp.py"
    fake.write_text(
        "import json, pathlib, sys\n"
        f"marker = pathlib.Path({str(marker)!r})\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    method = message.get('method')\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        # The legacy lifecycle works here, so a tool would appear if uagent
        # fell back to it. Only server/discover is refused.
        "    if method == 'initialize':\n"
        "        marker.write_text('called', encoding='utf-8')\n"
        "        result = {'protocolVersion': '2025-11-25', "
        "'capabilities': {'tools': {}}, "
        "'serverInfo': {'name': 'old', 'version': '1'}}\n"
        "        reply = {'jsonrpc': '2.0', 'id': message['id'], "
        "'result': result}\n"
        "    elif method == 'tools/list':\n"
        "        tools = [{'name': 'echo', 'description': 'legacy echo', "
        "'inputSchema': {'type': 'object', 'properties': {}}}]\n"
        "        reply = {'jsonrpc': '2.0', 'id': message['id'], "
        "'result': {'tools': tools}}\n"
        "    else:\n"
        "        reply = {'jsonrpc': '2.0', 'id': message['id'], "
        "'error': {'code': -32601, 'message': 'method not found'}}\n"
        "    print(json.dumps(reply), flush=True)\n",
        encoding="utf-8",
    )
    (workspace / ".mcp.json").write_text(
        json.dumps(
            {
                "mcpServers": {
                    "old": {
                        "command": sys.executable,
                        "args": [str(fake)],
                        "required": False,
                    }
                }
            }
        ),
        encoding="utf-8",
    )

    def verify(_, body):
        return event(
            {
                "content": "legacy-absent"
                if "old_echo" not in function_names(body)
                else "legacy-present"
            }
        )

    with Server([verify]) as server:
        result = run(
            workspace,
            base_env(home, server.url),
            "--trust-project-config",
            "--yolo",
            "-p",
            "check",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "legacy-absent", result.stdout)
        assert_true(not marker.exists(), "legacy initialize fallback was attempted")


def test_required_mcp_failure_stops_bootstrap(root, home):
    workspace = root / "mcp-required-failure"
    workspace.mkdir()
    fake = workspace / "fake_mcp.py"
    fake.write_text(
        "import json, sys\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    reply = {'jsonrpc': '2.0', 'id': message['id'], "
        "'error': {'code': -32601, 'message': 'unsupported'}}\n"
        "    print(json.dumps(reply), flush=True)\n",
        encoding="utf-8",
    )
    (workspace / ".mcp.json").write_text(
        json.dumps({"mcpServers": {"needed": {"command": sys.executable, "args": [str(fake)]}}}),
        encoding="utf-8",
    )

    with Server([event({"content": "must-not-run"})]) as server:
        result = run(
            workspace,
            base_env(home, server.url),
            "--trust-project-config",
            "-p",
            "check",
        )
        assert_true(result.returncode != 0, result.stdout)
        assert_true("required MCP server `needed`" in result.stderr, result.stderr)
        assert_true(not server.requests, server.requests)


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
        "    meta = message.get('params', {}).get('_meta', {})\n"
        "    assert meta.get('io.modelcontextprotocol/protocolVersion') == '2026-07-28'\n"
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
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
        result = tool_results(body["messages"])[0]
        return event({"content": "mcp-ok" if "mcp:hello" in result else "mcp-bad"})

    with Server([tool_call("probe_echo", {"text": "hello"}), final]) as server:
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


def test_optional_mcp_servers_share_startup_grace(root, home):
    workspace = root / "mcp-optional-startup"
    workspace.mkdir()
    slow = workspace / "slow_mcp.py"
    slow.write_text(
        "import sys, time\nfor _ in sys.stdin:\n    time.sleep(60)\n",
        encoding="utf-8",
    )
    fast = workspace / "fast_mcp.py"
    fast.write_text(
        "import json, sys\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    method = message.get('method')\n"
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'echo', 'description': 'echo', "
        "'inputSchema': {'type': 'object', 'properties': {}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        result = {'content': [{'type': 'text', 'text': 'fast'}]}\n"
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
                    "slow": {
                        "command": sys.executable,
                        "args": [str(slow)],
                        "required": False,
                    },
                    "fast": {
                        "command": sys.executable,
                        "args": [str(fast)],
                        "required": False,
                    },
                }
            }
        ),
        encoding="utf-8",
    )

    def final(_, body):
        result = tool_results(body["messages"])[0]
        return event({"content": "optional-ok" if "fast" in result else "optional-bad"})

    with Server([tool_call("fast_echo", {}), final]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MCP_STARTUP_GRACE"] = "1"
        env["UAGENT_MCP_TIMEOUT"] = "8"
        started = time.monotonic()
        result = run(
            workspace,
            env,
            "--trust-project-config",
            "--yolo",
            "-p",
            "probe",
        )
        elapsed = time.monotonic() - started
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "optional-ok", result.stdout)
        assert_true(elapsed < budget(4), f"optional startup took {elapsed:.2f}s")


def test_optional_mcp_refresh_never_blocks_a_model_step(root, home):
    workspace = root / "mcp-optional-nonblocking"
    workspace.mkdir()
    fake = workspace / "fake_mcp.py"
    write_mcp_server(
        fake,
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        time.sleep(60)\n"
        "        continue\n",
        extra_imports=("time",),
    )
    (workspace / ".mcp.json").write_text(
        json.dumps(
            {
                "mcpServers": {
                    "pending": {
                        "command": sys.executable,
                        "args": [str(fake)],
                        "required": False,
                    }
                }
            }
        ),
        encoding="utf-8",
    )

    rounds = 0

    def route(_, __):
        nonlocal rounds
        rounds += 1
        if rounds <= 3:
            return tool_call("read_path", {"path": "."})
        return event({"content": "nonblocking-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MCP_STARTUP_GRACE"] = "1"
        started = time.monotonic()
        result = run(
            workspace,
            env,
            "--trust-project-config",
            "--yolo",
            "-p",
            "probe",
        )
        elapsed = time.monotonic() - started
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "nonblocking-ok", result.stdout)
        assert_true(elapsed < budget(3), f"optional refresh blocked for {elapsed:.2f}s")


def test_optional_mcp_notification_before_discovery_is_not_fatal(root, home):
    workspace = root / "mcp-optional-notification"
    workspace.mkdir()
    fake = workspace / "fake_mcp.py"
    fake.write_text(
        "import json, sys, time\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    method = message.get('method')\n"
        "    if method == 'server/discover':\n"
        "        print(json.dumps({'jsonrpc': '2.0', 'method': "
        "'notifications/message', 'params': {'message': 'booting'}}), flush=True)\n"
        "        time.sleep(1.3)\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'echo', 'description': 'echo', "
        "'inputSchema': {'type': 'object', 'properties': {}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        result = {'content': [{'type': 'text', 'text': 'notice-tool'}]}\n"
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
                    "notice": {
                        "command": sys.executable,
                        "args": [str(fake)],
                        "required": False,
                    }
                }
            }
        ),
        encoding="utf-8",
    )

    def route(_, body):
        results = tool_results(body["messages"])
        if any("notice-tool" in result for result in results):
            return event({"content": "notification-ok"})
        if "notice_echo" in function_names(body):
            return tool_call("notice_echo", {})
        if len(results) >= 2:
            return event({"content": "notification-missing"})
        return tool_call("run", {"command": "sleep 0.5"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MCP_STARTUP_GRACE"] = "1"
        result = run(
            workspace,
            env,
            "--trust-project-config",
            "--yolo",
            "-p",
            "probe",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "notification-ok", result.stdout)


def test_optional_mcp_slow_tool_list_resumes_one_request(root, home):
    workspace = root / "mcp-optional-slow-tools"
    workspace.mkdir()
    requests = workspace / "tools-list-count"
    delayed = workspace / "delayed_mcp.py"
    delayed.write_text(
        "import json, pathlib, sys, time\n"
        f"count = pathlib.Path({str(requests)!r})\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    method = message.get('method')\n"
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        count.write_text(count.read_text() + '1' if count.exists() else '1')\n"
        "        time.sleep(2.2)\n"
        "        result = {'tools': [{'name': 'echo', 'description': 'echo', "
        "'inputSchema': {'type': 'object', 'properties': {}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        result = {'content': [{'type': 'text', 'text': 'slow-tool'}]}\n"
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
                    "delayed": {
                        "command": sys.executable,
                        "args": [str(delayed)],
                        "required": False,
                    }
                }
            }
        ),
        encoding="utf-8",
    )

    def route(_, body):
        results = tool_results(body["messages"])
        if any("slow-tool" in result for result in results):
            return event({"content": "delayed-ready"})
        if "delayed_echo" in function_names(body):
            return tool_call("delayed_echo", {})
        if results:
            return event({"content": "delayed-missing"})
        return tool_call("run", {"command": "sleep 1.5"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MCP_STARTUP_GRACE"] = "1"
        env["UAGENT_MCP_TIMEOUT"] = "8"
        result = run(
            workspace,
            env,
            "--trust-project-config",
            "--yolo",
            "-p",
            "probe",
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "delayed-ready", result.stdout)
        assert_true(requests.read_text() == "1", requests.read_text())
