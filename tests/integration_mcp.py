import base64
import hashlib
import json
import sys
import time

from integration_support import (
    SMALL_PNG,
    Server,
    assert_true,
    base_env,
    budget,
    event,
    function_names,
    mcp_servers,
    run,
    run_dialog,
    run_pty,
    tool_call,
    tool_results,
    write_mcp_server,
)
from web_support import web_host


def test_mcp_roots_continuation_preserves_arguments(root, home, *, binary):
    workspace = root / "mcp-roots-continuation"
    workspace.mkdir()
    fake = workspace / "server.py"
    write_mcp_server(
        fake,
        "    assert message.get('jsonrpc') == '2.0'\n"
        "    params = message.get('params', {})\n"
        "    assert params['_meta']['io.modelcontextprotocol/clientCapabilities'] == {'roots': {}, "
        "'extensions': {'io.modelcontextprotocol/tasks': {}}}\n"
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], 'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'echo', 'inputSchema': {'type': 'object', "
        "'properties': {'value': {'type': 'string'}}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        assert params['arguments'] == {'value': ''}\n"
        "        if 'requestState' not in params:\n"
        "            initial_id = message['id']\n"
        "            result = {'resultType': 'input_required', 'requestState': 'opaque-state', "
        "'inputRequests': {'scope': {'method': 'roots/list'}}}\n"
        "        else:\n"
        "            assert message['id'] != initial_id\n"
        "            assert params['requestState'] == 'opaque-state'\n"
        "            assert params['inputResponses']['scope']['roots'][0]['uri'] == ROOT\n"
        "            result = {'content': [{'type': 'text', 'text': 'roots-and-empty-ok'}]}\n"
        "    result.setdefault('resultType', 'complete')\n",
        setup=f"ROOT = {workspace.resolve().as_uri()!r}\n",
    )
    mcp_servers(workspace, {"probe": fake})

    def verify(_, body):
        assert_true("roots-and-empty-ok" in "".join(tool_results(body["messages"])), body)
        return event({"content": "ok"})

    with Server([tool_call("probe_echo", {"value": ""}), verify]) as server:
        result = run(
            workspace,
            base_env(home, server.url),
            "--trust-project-config",
            "--yolo",
            "-p",
            "exercise roots",
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)


FORM = {
    "type": "object",
    "required": ["name"],
    "properties": {
        "name": {"type": "string", "title": "Name"},
        "age": {"type": "integer", "title": "Age", "default": 30},
    },
}


def write_elicit_server(home, workspace, cases, delays=None):
    """An MCP server whose `ask` tool asks `cases[case]` (key -> elicitation)
    in one round and returns the inputResponses it got back. `delays[case]`
    holds seconds to wait before the first answer and before the result.
    Each retry is logged to `retries`."""
    fake = workspace / "server.py"
    write_mcp_server(
        fake,
        "    params = message.get('params', {})\n"
        "    capabilities = params['_meta']['io.modelcontextprotocol/clientCapabilities']\n"
        "    assert capabilities['elicitation'] == {'form': {}, 'url': {}}\n"
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], 'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'ask', 'inputSchema': {'type': 'object', "
        "'properties': {'case': {'type': 'string'}}}}]}\n"
        "    elif method != 'tools/call':\n"
        "        result = {}\n"
        "    else:\n"
        "        case = params['arguments']['case']\n"
        "        retry = 'inputResponses' in params\n"
        "        time.sleep(DELAYS.get(case, (0, 0))[retry])\n"
        "        if retry:\n"
        "            with open(RETRIES, 'a') as log:\n"
        "                log.write(case + '\\n')\n"
        "            answer = json.dumps(params['inputResponses'], sort_keys=True)\n"
        "            result = {'content': [{'type': 'text', 'text': 'got ' + answer}]}\n"
        "        else:\n"
        "            requests = {key: {'method': 'elicitation/create', 'params': value} "
        "for key, value in CASES[case].items()}\n"
        "            result = {'resultType': 'input_required', 'inputRequests': requests}\n"
        "    result.setdefault('resultType', 'complete')\n",
        setup=f"CASES = {cases!r}\nDELAYS = {delays or {}!r}\n"
        f"RETRIES = {str(workspace / 'retries')!r}\n",
        extra_imports=("time",),
    )
    (home / ".mcp.json").write_text(
        json.dumps({"mcpServers": {"probe": {"command": sys.executable, "args": [str(fake)]}}})
    )
    return workspace / "retries"


def test_mcp_elicitation_is_answered_in_the_terminal(root, home, *, binary):
    """Form and link requests are put to the person: accepted, declined at a
    field, cancelled; a cancel ends its round unseen and an unshowable form is
    declined with a note. mcp.timeout bounds each request, not the call, and
    not the retry after an accepted link."""
    workspace = root / "mcp-elicit"
    workspace.mkdir()
    write_elicit_server(
        home,
        workspace,
        {
            "form": {"q": {"message": "Form A?", "requestedSchema": FORM}},
            "decline": {"q": {"message": "Form B?", "requestedSchema": FORM}},
            "url": {"q": {"mode": "url", "message": "Link C", "url": "https://login.example/c"}},
            "pair": {
                "a": {"message": "Pair E1", "requestedSchema": FORM},
                "b": {"message": "Pair E2", "requestedSchema": FORM},
            },
            "nested": {
                "q": {
                    "message": "Nested",
                    "requestedSchema": {
                        "type": "object",
                        "properties": {"home": {"type": "object"}},
                    },
                }
            },
        },
        # Together longer than mcp.timeout, each request within it.
        {"form": (2, 2), "url": (0, 4)},
    )
    order = ["form", "decline", "url", "pair", "nested"]

    def route(_, body):
        results = tool_results(body["messages"])
        if len(results) < len(order):
            return tool_call("probe_ask", {"case": order[len(results)]}, call_id=f"c{len(results)}")
        expected = [
            'got {"q": {"action": "accept", "content": {"age": 30, "name": "Ada"}}}',
            'got {"q": {"action": "decline"}}',
            'got {"q": {"action": "accept"}}',
            'got {"a": {"action": "cancel"}, "b": {"action": "cancel"}}',
            'got {"q": {"action": "decline"}}',
        ]
        fine = all(want in got for want, got in zip(expected, results, strict=True))
        fine = fine and "declined probe's request for input" in results[-1]
        return event({"content": "elicit-ok" if fine else "elicit-bad"})

    accepted = b"probe \xe2\x86\x92 accepted"
    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MCP_TIMEOUT"] = "3"
        status, output = run_pty(
            workspace,
            env,
            [
                (b"go\n", b"Form A?"),
                (b"\r", b"Name"),
                (b"Ada\r", b"[a] Accept"),
                (b"a\r", accepted, b"Form B?", None),
                (b"/decline\r", b"probe \xe2\x86\x92 declined", b"Link C", None),
                (b"a\r", accepted, b"Pair E1", None),
                (b"\x1b", b"elicit-", b"Ready", None),
                b"/quit\n",
            ],
            timeout=40,
            args=("--yolo",),
            binary=binary,
        )
    assert_true(status == 0, output)
    assert_true(b"elicit-ok" in output, output)
    assert_true(b"Pair E2" not in output, output)
    assert_true(b"It goes to \x1b[1mlogin.example" in output, output)


def test_mcp_elicitation_ends_with_a_stopped_turn(root, home, *, binary):
    """Stopping the turn while a card is open closes it: nothing is sent back
    to the server, and the conversation is free again."""
    workspace = root / "mcp-elicit-stop"
    workspace.mkdir()
    retries = write_elicit_server(
        home, workspace, {"form": {"q": {"message": "Form?", "requestedSchema": FORM}}}
    )

    def route(_, body):
        if tool_results(body["messages"]):
            return event({"content": "after"})
        return tool_call("probe_ask", {"case": "form"})

    with Server([route]) as server, web_host(binary, root, home, server.url) as (web, code, _, _):
        web.pair(code)
        session = web.create(workspace)
        web.command("permissions", session, mode="yolo")
        web.command("submit", session, text="go")
        asked = web.until(session, lambda value: bool(value.get("pending")))
        assert_true(asked["pending"]["kind"] == "elicit", asked["pending"])
        web.command("interrupt", session)
        ended = web.until(session, lambda value: value["metadata"]["status"] == "idle")
        assert_true(not ended.get("pending"), ended)
    assert_true(not retries.exists(), retries.read_text() if retries.exists() else "")


TASK_SERVER = """
INTERVAL = {'done': 300, 'fail': 1, 'ask': 300, 'hang': 300}
polls, answered = {}, {}


def task(case, **fields):
    return {'taskId': case, 'createdAt': '2026-07-28T00:00:00Z',
            'lastUpdatedAt': '2026-07-28T00:00:00Z', 'ttlMs': None,
            'pollIntervalMs': INTERVAL[case], **fields}


def step(case, n):
    if case == 'done' and n < 3:
        return task(case, status='working', statusMessage='step %d' % n)
    if case == 'done':
        text = 'done after %d polls' % n
        return task(case, status='completed', result={'content': [{'type': 'text', 'text': text}]})
    if case == 'fail':
        return task(case, status='failed', error={'code': -32603, 'message': 'rate limit hit'})
    if case == 'hang':
        return task(case, status='working', statusMessage='still crunching')
    # The poll right after tasks/update still shows the answered request.
    if case not in answered or n <= answered[case][1] + 1:
        form = {'type': 'object', 'properties': {'name': {'type': 'string'}}}
        asked = {'q': {'method': 'elicitation/create',
                       'params': {'message': 'Task asks?', 'requestedSchema': form}}}
        return task(case, status='input_required', inputRequests=asked)
    text = 'got ' + json.dumps(answered[case][0], sort_keys=True)
    return task(case, status='completed', result={'content': [{'type': 'text', 'text': text}]})
"""


def write_task_server(home, workspace):
    """An MCP server whose `job` tool answers as a task named by its `case`;
    every request is logged with its arrival time."""
    fake = workspace / "server.py"
    log = workspace / "requests"
    write_mcp_server(
        fake,
        "    params = message.get('params', {})\n"
        "    capabilities = params['_meta']['io.modelcontextprotocol/clientCapabilities']\n"
        "    with open(LOG, 'a') as out:\n"
        "        case = params.get('taskId') or params.get('arguments', {}).get('case')\n"
        "        out.write(json.dumps({'method': method, 'task': case, "
        "'at': time.monotonic(), 'declared': capabilities['extensions'] == "
        "{'io.modelcontextprotocol/tasks': {}}}) + '\\n')\n"
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], 'capabilities': "
        "{'tools': {}, 'extensions': {'io.modelcontextprotocol/tasks': {}}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'job', 'inputSchema': {'type': 'object', "
        "'properties': {'case': {'type': 'string'}}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        case = params['arguments']['case']\n"
        "        polls[case] = 0\n"
        "        result = dict(task(case, status='working'), resultType='task')\n"
        "    elif method == 'tasks/get':\n"
        "        polls[params['taskId']] += 1\n"
        "        result = step(params['taskId'], polls[params['taskId']])\n"
        "    elif method == 'tasks/update':\n"
        "        answered[params['taskId']] = (params['inputResponses'], polls[params['taskId']])\n"
        "        result = {}\n"
        "    else:\n"
        "        result = {}\n"
        "    result.setdefault('resultType', 'complete')\n",
        setup=f"LOG = {str(log)!r}\n{TASK_SERVER}",
        extra_imports=("time",),
    )
    (home / ".mcp.json").write_text(
        json.dumps({"mcpServers": {"probe": {"command": sys.executable, "args": [str(fake)]}}})
    )

    def requests(case=None):
        rows = [json.loads(line) for line in log.read_text().splitlines()]
        return [row for row in rows if case is None or row["task"] == case]

    return requests


def test_mcp_task_is_followed_to_its_end(root, home, *, binary):
    """A tools/call answered as a task is polled at the server's interval
    (floored) until it ends: completed, failed, asking the person once per
    request, and cancelled once when the person stops it."""
    workspace = root / "mcp-task"
    workspace.mkdir()
    requests = write_task_server(home, workspace)
    order = ["done", "fail", "ask", "hang"]
    seen = []

    def route(_, body):
        results = tool_results(body["messages"])
        seen[:] = results
        if len(results) < len(order):
            return tool_call("probe_job", {"case": order[len(results)]}, call_id=f"c{len(results)}")
        return event({"content": "unexpected"})

    with Server([route]) as server, web_host(binary, root, home, server.url) as (web, code, _, _):
        web.pair(code)
        session = web.create(workspace)
        web.command("permissions", session, mode="yolo")
        web.command("submit", session, text="go")
        asked = web.until(session, lambda value: bool(value.get("pending")))
        assert_true(asked["pending"]["kind"] == "elicit", asked["pending"])
        answer = {"action": "accept", "content": {"name": "Ada"}}
        web.command(
            "reply", session, interaction_id=asked["pending"]["id"], text=json.dumps(answer)
        )
        web.until(session, lambda value: "probe: still crunching" in value["state"]["turn_status"])
        web.command("interrupt", session)
        web.until(session, lambda value: value["metadata"]["status"] == "idle")
    assert_true(len(seen) == 3, seen)
    assert_true("done after 3 polls" in seen[0], seen)
    assert_true("rate limit hit" in seen[1], seen)
    assert_true('got {"q": {"action": "accept", "content": {"name": "Ada"}}}' in seen[2], seen)
    rows = requests()
    assert_true(all(row["declared"] for row in rows), rows)
    for case, floor in (("done", 0.3), ("fail", 0.25)):
        times = [row["at"] for row in requests(case)] or [0]
        gaps = [later - earlier for earlier, later in zip(times, times[1:], strict=False)]
        assert_true(gaps and min(gaps) >= floor, (case, gaps))
    methods = {case: [row["method"] for row in requests(case)] for case in order}
    assert_true(methods["done"] == ["tools/call"] + ["tasks/get"] * 3, methods)
    asked_once = ["tools/call", "tasks/get", "tasks/update", "tasks/get", "tasks/get"]
    assert_true(methods["ask"] == asked_once, methods)
    assert_true(methods["hang"].count("tasks/cancel") == 1, methods)
    assert_true(methods["hang"][-1] == "tasks/cancel", methods)
    assert_true(all("tasks/cancel" not in methods[case] for case in order[:3]), methods)


def test_mcp_task_is_cancelled_at_the_turn_deadline(root, home, *, binary):
    workspace = root / "mcp-task-deadline"
    workspace.mkdir()
    requests = write_task_server(home, workspace)

    def route(_, body):
        if tool_results(body["messages"]):
            return event({"content": "after"})
        return tool_call("probe_job", {"case": "hang"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MAX_TURN_SECONDS"] = "2"
        run(workspace, env, "--yolo", "-p", "go", binary=binary)
    methods = [row["method"] for row in requests("hang")]
    assert_true(methods.count("tasks/cancel") == 1 and methods[-1] == "tasks/cancel", methods)


def test_mcp_image_reaches_the_model(root, home, *, binary):
    """A tool result is text-only, so an MCP image reaches the model as an
    attachment on the next request."""
    workspace = root / "mcp-image"
    workspace.mkdir()
    png = base64.b64encode(SMALL_PNG).decode()
    fake = workspace / "fake_mcp.py"
    write_mcp_server(
        fake,
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
        "        result = {}\n",
        setup=f"PNG = {png!r}\n",
    )
    mcp_servers(workspace, {"cam": fake})

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
        quiet = "displayed inline" not in tool_text
        return event({"content": "image-ok" if (got_image and saved and quiet) else "image-bad"})

    with Server([tool_call("cam_shot", {}), verify]) as server:
        env = base_env(home, server.url)
        result = run(
            workspace, env, "--trust-project-config", "--yolo", "-p", "screenshot", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "image-ok", result.stdout + result.stderr)


def test_invalid_mcp_config_not_executed(root, home, *, binary):
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
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "ok", result.stdout)
        assert_true(not marker.exists(), "invalid MCP server config executed")


def test_legacy_mcp_server_is_rejected_without_initialize_fallback(root, home, *, binary):
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
    mcp_servers(workspace, {"old": fake}, required=False)

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
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "legacy-absent", result.stdout)
        assert_true(not marker.exists(), "legacy initialize fallback was attempted")


def test_required_mcp_failure_stops_bootstrap(root, home, *, binary):
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
    mcp_servers(workspace, {"needed": fake})

    with Server([event({"content": "must-not-run"})]) as server:
        result = run(
            workspace,
            base_env(home, server.url),
            "--trust-project-config",
            "-p",
            "check",
            binary=binary,
        )
        assert_true(result.returncode != 0, result.stdout)
        assert_true("required MCP server `needed`" in result.stderr, result.stderr)
        assert_true(not server.requests, server.requests)


def test_mcp_log_bound_does_not_limit_server_files(root, home, *, binary):
    workspace = root / "mcp-log-bound"
    workspace.mkdir()
    artifact = workspace / "artifact.bin"
    fake = workspace / "fake_mcp.py"
    write_mcp_server(
        fake,
        "    if method == 'server/discover':\n"
        f"        open({str(artifact)!r}, 'wb').write(b'x' * 65536)\n"
        "        sys.stderr.write('noise ' * 20000)\n"
        "        sys.stderr.flush()\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': []}\n"
        "    else:\n"
        "        result = {}\n",
    )
    mcp_servers(workspace, {"probe": fake})
    with Server([event({"content": "done"})]) as server:
        env = base_env(home, server.url)
        result = run(workspace, env, "--trust-project-config", "-p", "probe", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
    # The server's own files are unbounded; only its stderr goes through the
    # bounded log pump (test_log_pump_rotates_within_its_bound).
    assert_true(artifact.stat().st_size == 65536, artifact.stat().st_size)
    logs = list((home / ".uagent" / "mcp").glob("probe-*.log*"))
    assert_true(logs, "no MCP stderr log")
    log_bytes = sum(path.stat().st_size for path in logs)
    assert_true(log_bytes >= len("noise ") * 20000, log_bytes)


def test_optional_mcp_servers_share_startup_grace(root, home, *, binary):
    workspace = root / "mcp-optional-startup"
    workspace.mkdir()
    slow = workspace / "slow_mcp.py"
    slow.write_text(
        "import sys, time\nfor _ in sys.stdin:\n    time.sleep(60)\n",
        encoding="utf-8",
    )
    fast = workspace / "fast_mcp.py"
    write_mcp_server(
        fast,
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'echo', 'description': 'echo', "
        "'inputSchema': {'type': 'object', 'properties': {}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        result = {'content': [{'type': 'text', 'text': 'fast'}]}\n"
        "    else:\n"
        "        result = {}\n",
    )
    mcp_servers(workspace, {"slow": slow, "fast": fast}, required=False)

    def final(_, body):
        result = tool_results(body["messages"])[0]
        return event({"content": "optional-ok" if "fast" in result else "optional-bad"})

    with Server([tool_call("fast_echo", {}), final]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MCP_TIMEOUT"] = "8"
        started = time.monotonic()
        result = run(
            workspace, env, "--trust-project-config", "--yolo", "-p", "probe", binary=binary
        )
        elapsed = time.monotonic() - started
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "optional-ok", result.stdout)
        assert_true(elapsed < budget(5), f"optional startup took {elapsed:.2f}s")


def test_optional_mcp_refresh_never_blocks_a_model_step(root, home, *, binary):
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
    mcp_servers(workspace, {"pending": fake}, required=False)

    rounds = 0

    def route(_, __):
        nonlocal rounds
        rounds += 1
        if rounds <= 3:
            return tool_call("read_path", {"path": "."})
        return event({"content": "nonblocking-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        started = time.monotonic()
        result = run(
            workspace, env, "--trust-project-config", "--yolo", "-p", "probe", binary=binary
        )
        elapsed = time.monotonic() - started
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "nonblocking-ok", result.stdout)
        assert_true(elapsed < budget(4), f"optional refresh blocked for {elapsed:.2f}s")


def test_optional_mcp_notification_before_discovery_is_not_fatal(root, home, *, binary):
    workspace = root / "mcp-optional-notification"
    workspace.mkdir()
    fake = workspace / "fake_mcp.py"
    write_mcp_server(
        fake,
        "    if method == 'server/discover':\n"
        "        print(json.dumps({'jsonrpc': '2.0', 'method': "
        "'notifications/message', 'params': {'message': 'booting'}}), flush=True)\n"
        "        time.sleep(2.3)\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'echo', 'description': 'echo', "
        "'inputSchema': {'type': 'object', 'properties': {}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        result = {'content': [{'type': 'text', 'text': 'notice-tool'}]}\n"
        "    else:\n"
        "        result = {}\n",
        extra_imports=("time",),
    )
    mcp_servers(workspace, {"notice": fake}, required=False)

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
        result = run(
            workspace, env, "--trust-project-config", "--yolo", "-p", "probe", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "notification-ok", result.stdout)


def test_optional_mcp_slow_tool_list_resumes_one_request(root, home, *, binary):
    workspace = root / "mcp-optional-slow-tools"
    workspace.mkdir()
    requests = workspace / "tools-list-count"
    delayed = workspace / "delayed_mcp.py"
    write_mcp_server(
        delayed,
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        count.write_text(count.read_text() + '1' if count.exists() else '1')\n"
        "        time.sleep(3.2)\n"
        "        result = {'tools': [{'name': 'echo', 'description': 'echo', "
        "'inputSchema': {'type': 'object', 'properties': {}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        result = {'content': [{'type': 'text', 'text': 'slow-tool'}]}\n"
        "    else:\n"
        "        result = {}\n",
        extra_imports=("pathlib", "time"),
        setup=f"count = pathlib.Path({str(requests)!r})\n",
    )
    mcp_servers(workspace, {"delayed": delayed}, required=False)

    def route(_, body):
        results = tool_results(body["messages"])
        if any("slow-tool" in result for result in results):
            return event({"content": "delayed-ready"})
        if "delayed_echo" in function_names(body):
            return tool_call("delayed_echo", {})
        if results:
            return event({"content": "delayed-missing"})
        return tool_call("run", {"command": "sleep 2.5"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MCP_TIMEOUT"] = "8"
        result = run(
            workspace, env, "--trust-project-config", "--yolo", "-p", "probe", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "delayed-ready", result.stdout)
        assert_true(requests.read_text() == "1", requests.read_text())


SKILL_FILES = {
    "skill://acme/review/SKILL.md": "---\nname: review\ndescription: Review a change\n"
    "metadata:\n  owner: team\n---\n\nREVIEW STEPS: read references/guide.md.\n",
    "skill://acme/review/references/guide.md": "GUIDE BODY\n",
    "skill://broken/SKILL.md": "---\nname: broken\ndescription: Listed wrongly\n---\n\nX\n",
    "skill://renamed/SKILL.md": "---\nname: other\ndescription: Renamed\n---\n\nX\n",
    "skill://blob/SKILL.md": "---\nname: blob\ndescription: As a blob\n---\n\nX\n",
    "skill://moved/SKILL.md": "---\nname: moved\ndescription: Elsewhere\n---\n\nX\n",
}


def write_skill_server(home, workspace, required=True, skills_delay=0, skills_error=False):
    """An MCP server serving, over two pages, `acme/review` with a supporting
    file; `broken` whose listed digest is wrong; `renamed` whose SKILL.md names
    another skill; `blob` sent as a blob and `moved` under another uri; and,
    left out, a generated `daily`, a 17 MiB `big` and a 513-file `many`.
    Requests are logged."""

    def manifest(*uris, wrong=False):
        return [
            {
                "uri": uri,
                "digest": "sha256:"
                + hashlib.sha256((SKILL_FILES[uri] + ("!" if wrong else "")).encode()).hexdigest(),
                "size": len(SKILL_FILES[uri].encode()),
            }
            for uri in uris
        ]

    listed = [
        {
            "uri": "skill://acme/review/SKILL.md",
            "frontmatter": {
                "name": "review",
                "description": "Review a change",
                "metadata": {"owner": "team"},
            },
            "resources": manifest(
                "skill://acme/review/SKILL.md", "skill://acme/review/references/guide.md"
            ),
        },
        {
            "uri": "skill://broken/SKILL.md",
            "frontmatter": {"name": "broken", "description": "Listed wrongly"},
            "resources": manifest("skill://broken/SKILL.md", wrong=True),
        },
        {
            "uri": "skill://daily/SKILL.md",
            "frontmatter": {"name": "daily", "description": "Generated daily"},
            "resources": "dynamic",
        },
        *(
            {
                "uri": f"skill://{name}/SKILL.md",
                "frontmatter": {"name": name, "description": description},
                "resources": manifest(f"skill://{name}/SKILL.md"),
            }
            for name, description in (
                ("renamed", "Renamed"),
                ("blob", "As a blob"),
                ("moved", "Elsewhere"),
            )
        ),
        *(
            {
                "uri": f"skill://{name}/SKILL.md",
                "frontmatter": {"name": name, "description": "Too large"},
                "resources": [{**big, "uri": f"skill://{name}/{index}"} for index in range(count)]
                + [{**big, "uri": f"skill://{name}/SKILL.md"}],
            }
            for name, count, big in (
                ("big", 0, {"digest": "sha256:" + "0" * 64, "size": 17 << 20}),
                ("many", 512, {"digest": "sha256:" + "0" * 64, "size": 1}),
            )
        ),
    ]
    fake = workspace / "server.py"
    log = workspace / "requests"
    write_mcp_server(
        fake,
        "    params = message.get('params', {})\n"
        "    with open(LOG, 'a') as out:\n"
        "        out.write(json.dumps([method, params.get('uri')]) + '\\n')\n"
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], 'capabilities': {'tools': {}, "
        "'resources': {}, 'extensions': {'io.modelcontextprotocol/skills': {}}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'echo', 'inputSchema': {'type': 'object'}}]}\n"
        "    elif method == 'tools/call':\n"
        "        result = {'content': [{'type': 'text', 'text': 'echoed'}]}\n"
        "    elif method == 'skills/list' and ERROR:\n"
        "        print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], "
        "'error': {'code': -32601, 'message': 'no skills here'}}), flush=True)\n"
        "        continue\n"
        "    elif method == 'skills/list':\n"
        "        later = 'cursor' in params\n"
        "        time.sleep(0 if later else DELAY)\n"
        "        result = {'skills': LISTED[2:] if later else LISTED[:2], 'ttlMs': 0, "
        "'cacheScope': 'private'}\n"
        "        if not later:\n"
        "            result['nextCursor'] = 'next'\n"
        "    elif method == 'resources/read':\n"
        "        uri, text = params['uri'], FILES[params['uri']]\n"
        "        content = {'uri': uri + '?' if 'moved' in uri else uri, 'text': text}\n"
        "        if 'blob' in uri:\n"
        "            content = {'uri': uri, 'blob': base64.b64encode(text.encode()).decode()}\n"
        "        result = {'contents': [content]}\n"
        "    else:\n"
        "        result = {}\n"
        "    result.setdefault('resultType', 'complete')\n",
        setup=f"LOG = {str(log)!r}\nLISTED = {listed!r}\nFILES = {SKILL_FILES!r}\n"
        f"DELAY = {skills_delay!r}\nERROR = {skills_error!r}\n",
        extra_imports=("base64", "time"),
    )
    entry = {"command": sys.executable, "args": [str(fake)], "required": required}
    (home / ".mcp.json").write_text(json.dumps({"mcpServers": {"probe": entry}}))
    local = home / ".agents" / "skills" / "review"
    local.mkdir(parents=True)
    (local / "SKILL.md").write_text("---\ndescription: The local review\n---\n\nLOCAL REVIEW\n")

    def requests():
        return [tuple(json.loads(line)) for line in log.read_text().splitlines()]

    return requests


def test_mcp_served_skills_load_after_the_person_allows_them(root, home, *, binary):
    """Served skills join the catalogue as `<server>:<path>` beside a local
    skill of the same name; a load asks the person even in yolo, is checked
    against the manifest, and nothing is read before it is needed."""
    workspace = root / "mcp-skills"
    workspace.mkdir()
    requests = write_skill_server(home, workspace)
    guide = {"name": "probe:acme/review", "file": "references/guide.md"}
    calls = [
        guide,
        {"name": "review"},
        {"name": "probe:acme/review"},
        guide,
        {"name": "probe:broken"},
        {"name": "probe:acme/review", "file": "missing.md"},
        {"name": "probe:acme/review", "file": "../broken/SKILL.md"},
        {"name": "probe:renamed"},
        {"name": "probe:blob"},
        {"name": "probe:moved"},
        {"name": "probe:acme/review"},
    ]
    cards = ["broken", "renamed", "blob", "moved"]
    answers = [("probe:acme/review", "y"), *((f"probe:{n}", "y") for n in cards)]
    answers.append(("probe:acme/review", "n"))
    seen = {}

    def route(_, body):
        results = tool_results(body["messages"])
        if "Summarize the bounded transcript" in json.dumps(body["messages"][-1]):
            return event({"content": "summary"})
        if "results" in seen:
            seen["after"] = body["messages"]
            return event({"content": "after compaction"})
        if not results:
            skill = next(t for t in body["tools"] if t["function"]["name"] == "skill")
            seen["catalogue"] = skill["function"]["description"]
            seen["read_before"] = [r for r in requests() if r[0] == "resources/read"]
        if len(results) < len(calls):
            return tool_call("skill", calls[len(results)], call_id=f"c{len(results)}")
        seen["results"] = results
        return event({"content": "done"})

    with Server([route]) as server, web_host(binary, root, home, server.url) as (web, code, _, _):
        web.pair(code)
        session = web.create(workspace)
        web.command("permissions", session, mode="yolo")
        web.command("submit", session, text="go")
        for name, answer in answers:
            asked = web.until(session, lambda value: bool(value.get("pending")))
            shown = json.dumps(asked["pending"])
            assert_true(name in shown and "MCP server probe" in shown, shown)
            web.command("reply", session, interaction_id=asked["pending"]["id"], text=answer)
            web.until(
                session,
                lambda value, held=asked: (
                    (value.get("pending") or {}).get("id") != held["pending"]["id"]
                ),
            )
        web.until(session, lambda value: value["metadata"]["status"] == "idle")
        # Compaction carries a local skill on as harness text, never a served one.
        web.command("submit", session, text="/compact")
        web.until(session, lambda value: value["metadata"]["status"] == "idle")
        web.command("submit", session, text="next")
        web.until(session, lambda _: "after" in seen)
        web.until(session, lambda value: value["metadata"]["status"] == "idle")
    after = json.dumps(seen["after"])
    assert_true("kept across compaction" in after and "LOCAL REVIEW" in after, after)
    assert_true("REVIEW STEPS" not in after, after)
    catalogue = seen["catalogue"]
    assert_true("- review: The local review" in catalogue, catalogue)
    assert_true("- probe:acme/review: Review a change" in catalogue, catalogue)
    assert_true("probe:broken" in catalogue and "probe:moved" in catalogue, catalogue)
    left_out = ("daily", "probe:big", "probe:many")
    assert_true(not any(name in catalogue for name in left_out), catalogue)
    assert_true(seen["read_before"] == [], seen)
    results = seen["results"]
    assert_true("open probe:acme/review before its files" in results[0], results)
    assert_true("LOCAL REVIEW" in results[1], results)
    assert_true("served by MCP server probe" in results[2], results)
    assert_true("REVIEW STEPS" in results[2] and "owner" not in results[2], results)
    assert_true("GUIDE BODY" in results[3], results)
    unused = "does not match its listed size and digest"
    assert_true(unused in results[4], results)
    assert_true(all("is not a file of probe:acme/review" in r for r in results[5:7]), results)
    assert_true("differs from probe's listing" in results[7], results)
    assert_true(unused in results[8] and unused in results[9], results)
    assert_true("not approved" in results[10], results)
    reads = [r[1] for r in requests() if r[0] == "resources/read"]
    expected = ["skill://acme/review/SKILL.md", "skill://acme/review/references/guide.md"]
    expected += [f"skill://{name}/SKILL.md" for name in cards]
    assert_true(reads == expected, reads)


def test_mcp_served_skill_is_refused_with_nobody_to_ask(root, home, *, binary):
    workspace = root / "mcp-skills-headless"
    workspace.mkdir()
    # Optional: its skills arrive through the non-blocking startup.
    requests = write_skill_server(home, workspace, required=False)

    seen = []

    def route(_, body):
        if results := tool_results(body["messages"]):
            seen.extend(results)
            return event({"content": "after"})
        return tool_call("skill", {"name": "probe:acme/review"})

    with Server([route]) as server:
        result = run(workspace, base_env(home, server.url), "--yolo", "-p", "go", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
    assert_true("loading a skill an MCP server serves" in "".join(seen), seen)
    assert_true("nobody is here" in "".join(seen), seen)
    assert_true(all(r[0] != "resources/read" for r in requests()), requests())
    # The terminal's skill list names this conversation's served skills.
    listed = run_dialog(workspace, base_env(home, server.url), "/skills\n/q\n", binary=binary)
    assert_true('"name": "probe:acme/review"' in listed.stdout, listed.stdout)


def test_mcp_late_skills_join_while_another_server_works(root, home, *, binary):
    """An optional server whose skills/list answers after the startup window
    ends ready, with its tools and skills, at the step after another server's
    tool ran."""
    workspace = root / "mcp-skills-late"
    workspace.mkdir()
    write_skill_server(home, workspace, required=False, skills_delay=3)
    other = workspace / "other.py"
    write_mcp_server(
        other,
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], 'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'wait', 'inputSchema': {'type': 'object'}}]}\n"
        "    elif method == 'tools/call':\n"
        "        time.sleep(WAIT.pop() if WAIT else 0)\n"
        "        result = {'content': [{'type': 'text', 'text': 'waited'}]}\n"
        "    else:\n"
        "        result = {}\n",
        setup="WAIT = [3]\n",
        extra_imports=("time",),
    )
    config = json.loads((home / ".mcp.json").read_text())
    config["mcpServers"]["other"] = {"command": sys.executable, "args": [str(other)]}
    (home / ".mcp.json").write_text(json.dumps(config))
    offered = []

    def route(_, body):
        names = {t["function"]["name"]: t["function"] for t in body["tools"]}
        offered.append(("probe_echo" in names, "probe:acme/review" in json.dumps(names)))
        # Each step takes up one page of the late listing.
        if offered[-1] == (True, True) or len(offered) > 4:
            return event({"content": "after"})
        return tool_call("other_wait", {}, call_id=f"c{len(offered)}")

    with Server([route]) as server:
        result = run(workspace, base_env(home, server.url), "--yolo", "-p", "go", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
    assert_true(offered[0] == (False, False) and offered[-1] == (True, True), offered)


def test_mcp_failed_skills_list_keeps_the_server(root, home, *, binary):
    workspace = root / "mcp-skills-error"
    workspace.mkdir()
    write_skill_server(home, workspace, skills_error=True)
    seen = []

    def route(_, body):
        if results := tool_results(body["messages"]):
            seen.extend(results)
            return event({"content": "after"})
        return tool_call("probe_echo", {})

    with Server([route]) as server:
        result = run(workspace, base_env(home, server.url), "--yolo", "-p", "go", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
    assert_true(seen == ["echoed"], seen)
