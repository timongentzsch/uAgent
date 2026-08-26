import base64

from integration_support import (
    SMALL_PNG,
    Server,
    assert_true,
    base_env,
    event,
    function_names,
    json,
    run,
    sys,
    tool_call,
    tool_results,
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
