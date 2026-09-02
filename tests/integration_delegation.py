import os
import re

from integration_support import (
    BINARY,
    Server,
    assert_token_budget_stop,
    assert_true,
    base_env,
    descendant_pids,
    event,
    function_names,
    has_message,
    json,
    run,
    run_dialog,
    signal,
    subprocess,
    sys,
    threading,
    time,
    tool_call,
    tool_results,
    wait_for_processes_stopped,
    wait_until,
    write_http_response,
    write_mcp_server,
)


def test_subagent_usage_counts_toward_parent_token_budget(root, home):
    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child-budget"):
            return event(
                {"content": "child-result"},
                usage={"prompt_tokens": 2, "completion_tokens": 4},
            )
        return tool_call("subagent", {"prompt": "child-budget", "background": False})

    with Server([route]) as server:
        envelope = assert_token_budget_stop(
            root, home, server, "--yolo", "--token-budget", "3", "-p", "delegate"
        )
        assert_true(envelope["stop"]["session_generated_tokens"] == 4, envelope)
        assert_true(len(server.requests) == 2, server.requests)


def test_completed_parent_answer_survives_late_child_budget_usage(root, home):
    child_requested = threading.Event()

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "late-budget-child"):
            child_requested.set()
            return event(
                {"content": "too-expensive-child"},
                usage={"prompt_tokens": 1, "completion_tokens": 4},
            )
        results = tool_results(messages)
        if any("[started] subagent id " in result for result in results):
            assert_true(child_requested.wait(2), "child request did not arrive")
            time.sleep(0.3)
            return event({"content": "parent-answer"})
        return tool_call("subagent", {"prompt": "late-budget-child"})

    with Server([route]) as server:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            "--token-budget",
            "3",
            "--json",
            "-p",
            "delegate",
            timeout=10,
        )
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 0, envelope)
        assert_true(envelope["answer"] == "parent-answer", envelope)
        assert_true(envelope["stop"]["reason"] == "completed", envelope)
        assert_true(envelope["stop"]["session_generated_tokens"] == 4, envelope)


def test_subagent_inherits_only_remaining_session_token_budget(root, home):
    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child-remainder"):
            response = tool_call("read_path", {"path": "."})
            response["usage"] = {"prompt_tokens": 1, "completion_tokens": 3}
            return response
        response = tool_call("subagent", {"prompt": "child-remainder", "background": False})
        response["usage"] = {"prompt_tokens": 1, "completion_tokens": 3}
        return response

    with Server([route]) as server:
        assert_token_budget_stop(
            root, home, server, "--yolo", "--token-budget", "5", "-p", "delegate"
        )
        # The child inherited the two-token remainder and stopped before
        # executing its call or requesting a second model round.
        assert_true(len(server.requests) == 2, server.requests)


def test_subagent_auto_join_continues_turn(root, home):
    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            time.sleep(2)
            return event({"content": "child-result"})
        has_result = any(
            isinstance(message.get("content"), str) and "[Background result:" in message["content"]
            for message in messages
        )
        if has_result:
            return event({"content": "late-task-ok"})
        if any("[started] subagent id " in str(message.get("content", "")) for message in messages):
            return tool_call("activity", {"operation": "wait", "wait_ms": 30000})
        return tool_call("subagent", {"prompt": "child"})

    with Server([route]) as server:
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


def test_subagent_foreground_returns_result_without_wait_round(root, home):
    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            time.sleep(0.3)
            return event({"content": "foreground-child-result"})
        results = tool_results(messages)
        if any("foreground-child-result" in result for result in results):
            direct = all("[started] subagent id " not in result for result in results)
            return event({"content": "foreground-task-ok" if direct else "foreground-task-bad"})
        return tool_call("subagent", {"prompt": "child", "background": False})

    with Server([route]) as server:
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


def test_lean_subagent_does_not_clone_parent_mcp_fleet(root, home):
    marker = root / "mcp-starts"
    fake = root / "fake_mcp.py"
    write_mcp_server(
        fake,
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    else:\n"
        "        result = {'tools': []} if method == 'tools/list' else {}\n",
        extra_imports=("os", "pathlib"),
        setup=f"marker = pathlib.Path({str(marker)!r})\n"
        "with marker.open('a', encoding='utf-8') as output:\n"
        "    output.write(os.environ.get('UAGENT_DEPTH', '0') + '\\n')\n",
    )
    (home / ".mcp.json").write_text(
        json.dumps(
            {"mcpServers": {"parent-only": {"command": sys.executable, "args": [str(fake)]}}}
        ),
        encoding="utf-8",
    )

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            return event({"content": "lean-child-result"})
        if any("lean-child-result" in result for result in tool_results(messages)):
            return event({"content": "lean-mcp-ok"})
        return tool_call("subagent", {"prompt": "child", "background": False})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "delegate")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "lean-mcp-ok", result.stdout)
        starts = marker.read_text(encoding="utf-8").splitlines()
        assert_true(starts == ["0"], starts)


def test_subagent_followup_resumes_durable_conversation(root, home):
    def route(_, body):
        messages = body["messages"]
        child_prompts = [
            message.get("content") for message in messages if message.get("role") == "user"
        ]
        if "remember alpha" in child_prompts:
            directed_followup = any(
                prompt
                == "[collaborator directive]\nalways mention beta\n\nwhat did I ask you to remember?"
                for prompt in child_prompts
            )
            if directed_followup:
                return event({"content": "alpha-from-history-with-directive"})
            return event({"content": "stored alpha"})

        results = "\n".join(tool_results(messages))
        if "alpha-from-history-with-directive" in results:
            return event({"content": "persistent-collaborator-ok"})
        if "stored alpha" in results:
            match = re.search(r"\[collaborator (agent-[^;\]]+)", results)
            assert_true(match is not None, results)
            return tool_call(
                "subagent",
                {
                    "operation": "followup",
                    "agent_id": match.group(1),
                    "prompt": "what did I ask you to remember?",
                    "background": False,
                },
            )
        return tool_call(
            "subagent",
            {
                "prompt": "remember alpha",
                "directive": "always mention beta",
                "background": False,
            },
        )

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "collaborate")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "persistent-collaborator-ok", result.stdout)
        records = list((home / ".uagent" / "collaborators").glob("agent-*.json"))
        records = [path for path in records if not path.name.endswith(".session.json")]
        assert_true(len(records) == 1, records)
        state = json.loads(records[0].read_text(encoding="utf-8"))
        assert_true(state["directive"] == "always mention beta", state)
        # A collaborator has to know it is one: guidance can arrive mid-run as
        # an ordinary user message, which it cannot infer from its own prompt.
        session = records[0].with_name(records[0].stem + ".session.json")
        assert_true("[collaborator:" in session.read_text(encoding="utf-8"), session)


def test_completed_child_answer_survives_collaborator_save_failure(root, home):
    # chmod cannot deny root, so the save failure this test needs is not
    # reachable there.
    if os.geteuid() == 0:
        return
    collaborators = home / ".uagent" / "collaborators"

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "save-failure-child"):
            collaborators.mkdir(parents=True, exist_ok=True)
            collaborators.chmod(0o500)
            return event({"content": "valuable-child-answer"})
        results = tool_results(messages)
        if results:
            report = results[-1]
            valid = (
                "valuable-child-answer" in report
                and "collaborator metadata was not saved" in report
            )
            return event({"content": "save-warning-ok" if valid else "save-warning-bad"})
        return tool_call("subagent", {"prompt": "save-failure-child", "background": False})

    try:
        with Server([route]) as server:
            result = run(root, base_env(home, server.url), "--yolo", "-p", "delegate")
            assert_true(result.returncode == 0, result.stderr)
            assert_true(result.stdout.strip() == "save-warning-ok", result.stdout)
    finally:
        if collaborators.exists():
            collaborators.chmod(0o700)


def test_failed_followup_consumes_queued_guidance_after_launch(root, home):
    collaborator_id = {"value": ""}

    def route(_, body):
        messages = body["messages"]
        users = [
            str(message.get("content", "")) for message in messages if message.get("role") == "user"
        ]
        results = tool_results(messages)

        if users and users[-1] == "seed child":
            return event({"content": "seeded"})
        if users and "fail child" in users[-1]:
            return event({}, finish="content_filter")
        if users and users[-1] == "retry child":
            queued = sum(prompt.count("[queued guidance]") for prompt in users)
            valid = queued == 1 and "queued once" not in users[-1]
            return event({"content": "mailbox-cleared" if valid else "mailbox-repeated"})

        if has_message(messages, "user", "spawn coordinator"):
            if results:
                match = re.search(r"\[collaborator (agent-[^;\]]+)", results[-1])
                assert_true(match is not None, results[-1])
                collaborator_id["value"] = match.group(1)
                return event({"content": "spawned"})
            return tool_call("subagent", {"prompt": "seed child", "background": False})
        if has_message(messages, "user", "queue coordinator"):
            if results:
                return event({"content": "queued"})
            return tool_call(
                "subagent",
                {
                    "operation": "message",
                    "agent_id": collaborator_id["value"],
                    "prompt": "queued once",
                },
            )
        if has_message(messages, "user", "fail coordinator"):
            if results:
                return event({"content": "failure-observed"})
            return tool_call(
                "subagent",
                {
                    "operation": "followup",
                    "agent_id": collaborator_id["value"],
                    "prompt": "fail child",
                    "background": False,
                },
            )
        if has_message(messages, "user", "retry coordinator"):
            if results:
                valid = "mailbox-cleared" in results[-1]
                return event({"content": "mailbox-ok" if valid else "mailbox-bad"})
            return tool_call(
                "subagent",
                {
                    "operation": "followup",
                    "agent_id": collaborator_id["value"],
                    "prompt": "retry child",
                    "background": False,
                },
            )
        return event({"content": "unexpected-route"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        for prompt, expected in (
            ("spawn coordinator", "spawned"),
            ("queue coordinator", "queued"),
            ("fail coordinator", "failure-observed"),
            ("retry coordinator", "mailbox-ok"),
        ):
            result = run(root, env, "--yolo", "-p", prompt)
            assert_true(result.returncode == 0, result.stderr)
            assert_true(result.stdout.strip() == expected, result.stdout)
        # Guidance lives in files beside the record until it is delivered, so a
        # message that reached the child has to leave nothing behind.
        left = list((home / ".uagent" / "collaborators").glob("*.mail-*"))
        assert_true(not left, left)


def test_message_reaches_running_child(root, home):
    """A message to a running child is delivered without waiting for a followup."""
    scale = max(1, int(float(os.environ.get("UAGENT_TEST_TIMEOUT_SCALE", "1"))))

    def route(_, body):
        messages = body["messages"]
        users = [
            str(message.get("content", "")) for message in messages if message.get("role") == "user"
        ]
        if "wait-for-guidance" in users:
            if any("[parent guidance]" in user for user in users):
                return event({"content": "guidance-received"})
            if len(messages) > 30 * scale:
                return event({"content": "guidance-never-arrived"})
            # Idle in short steps: the drain runs between them, so the child
            # only has to still be alive when the message lands.
            return tool_call("run", {"command": "sleep 0.2"})
        combined = "\n".join(str(message.get("content", "")) for message in messages)
        if "guidance-received" in combined:
            return event({"content": "live-message-ok"})
        if "queued message for collaborator" in combined:
            return tool_call("activity", {"operation": "wait", "wait_ms": 30000})
        if "[started] subagent id" in combined:
            match = re.search(r"\[collaborator (agent-[^;\]]+)", combined)
            assert_true(match is not None, combined)
            return tool_call(
                "subagent",
                {"operation": "message", "agent_id": match.group(1), "prompt": "say banana"},
            )
        return tool_call("subagent", {"prompt": "wait-for-guidance", "background": True})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "coordinate", timeout=60)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "live-message-ok", result.stdout)
        collaborators = home / ".uagent" / "collaborators"
        sessions = list(collaborators.glob("agent-*.session.json"))
        assert_true(len(sessions) == 1, sessions)
        # The guidance became an ordinary user message in the child's own
        # conversation, which is what makes it steering rather than a note.
        # The record is JSON, so the wrapper's newline is escaped in the file.
        transcript = sessions[0].read_text(encoding="utf-8")
        assert_true("[parent guidance]\\nsay banana" in transcript, transcript[:2000])
        assert_true(not list(collaborators.glob("*.mail-*")), "delivered mail was left behind")


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
        if "[started] subagent id " in combined:
            return tool_call("activity", {"operation": "wait", "wait_ms": 30000})
        return event(
            {
                "tool_calls": [
                    {
                        "index": index,
                        "id": f"task-{index}",
                        "function": {
                            "name": "subagent",
                            "arguments": json.dumps({"prompt": child_prompt}),
                        },
                    }
                    for index, child_prompt in enumerate(("child-a", "child-b"))
                ]
            },
            finish="tool_calls",
        )

    with Server([route]) as server:
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
        assert_true(3 <= len(parent_requests) <= 4, len(parent_requests))
        for request in parent_requests:
            assert_true("subagent" in function_names(request), request)


def test_subagent_interrupt_reaps_child(root, home):
    """A soft interrupt during the tool batch must not orphan delegation."""
    batch = event(
        {
            "tool_calls": [
                {
                    "index": 0,
                    "id": "call-task",
                    "function": {
                        "name": "subagent",
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
        child_pids = set()

        def delegated_child_started():
            if not trace.exists():
                return False
            for line in trace.read_text().splitlines():
                event_data = json.loads(line)
                data = event_data.get("data", {})
                if (
                    event_data.get("event") == "tool_result"
                    and data.get("name") == "subagent"
                    and str(data.get("result", "")).startswith("[started] subagent id ")
                ):
                    child_pids.update(descendant_pids(process.pid))
                    return True
            return False

        wait_until(delegated_child_started, "delegated child did not start", timeout=4)
        assert_true(child_pids, "delegated child pid was not observable")
        process.send_signal(signal.SIGINT)
        process.communicate(timeout=8)
        survivors = wait_for_processes_stopped(child_pids)
        assert_true(not survivors, f"delegated child processes survived: {survivors}")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
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
        runtime = "\n".join(
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "user"
        )
        assert_true("[delegation: parent=" in runtime, runtime)
        return tool_call(
            "subagent",
            {
                "prompt": "child",
                "mode": "full",
                "model": "codex-local/child model",
            },
        )

    def finish(_, body):
        combined = "\n".join(str(message.get("content", "")) for message in body["messages"])
        return event({"content": "route-ok" if "child-route-ok" in combined else "route-bad"})

    parent = Server(
        [
            delegate,
            lambda *_: tool_call("activity", {"operation": "wait", "wait_ms": 30000}),
            finish,
        ]
    )
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


def test_subagent_failure_reports_route_stage_and_bounded_diagnostics(root, home):
    def reject_child(handler, body):
        assert_true(body.get("model") == "unsupported-model", body)
        payload = json.dumps(
            {
                "error": {
                    "message": "fixture endpoint rejects unsupported-model",
                    "type": "invalid_request_error",
                    "code": "model_not_found",
                }
            }
        ).encode()
        write_http_response(handler, payload, status=400)

    child = Server([reject_child])

    def route(_, body):
        results = tool_results(body["messages"])
        report = next((result for result in reversed(results) if "configured route:" in result), "")
        if report:
            valid = all(
                marker in report
                for marker in (
                    "configured route: failing/child -> failing/unsupported-model",
                    "@ 127.0.0.1",
                    "failure stage: child execution",
                    "remedy:",
                    "fallback: none",
                    "partial diagnostics:",
                    "fixture endpoint rejects unsupported-model",
                )
            )
            valid = valid and "child-secret-do-not-print" not in report and len(report) < 4000
            return event({"content": "child-diagnostic-ok" if valid else report})
        if any("[started] subagent id " in result for result in results):
            return tool_call("activity", {"operation": "wait", "wait_ms": 30000})
        return tool_call(
            "subagent",
            {"prompt": "fail on the configured route", "model": "failing/child"},
        )

    parent = Server([route])
    try:
        env = base_env(home, parent.url)
        env["UAGENT_PROVIDERS"] = json.dumps(
            {
                "failing": {
                    "base_url": child.url,
                    "api_key": "child-secret-do-not-print",
                    "models": {"child": {"id": "unsupported-model", "effort": "low"}},
                }
            }
        )
        result = run(root, env, "--yolo", "-p", "delegate failure", timeout=12)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "child-diagnostic-ok", result.stdout)
        assert_true(len(child.requests) == 1, len(child.requests))
        assert_true(len(parent.requests) == 3, len(parent.requests))
    finally:
        parent.close()
        child.close()


def test_subagent_recursion_is_depth_bounded(root, home):
    """Full agents honor depth; lean workers never expose recursive delegation."""

    def has_task(body):
        return "subagent" in function_names(body)

    for depth, cap, expected in (
        ("0", "2", True),
        ("1", "2", True),
        ("2", "2", False),
        ("0", "0", False),
    ):
        with Server([lambda _, body: event({"content": str(has_task(body))})]) as server:
            env = base_env(home, server.url)
            env["UAGENT_DEPTH"] = depth
            env["UAGENT_SUBAGENT_DEPTH"] = cap
            result = run(root, env, "-p", "probe")
            assert_true(result.returncode == 0, result.stderr)
            assert_true(
                result.stdout.strip() == str(expected),
                (depth, cap, expected, result.stdout),
            )

    with Server([lambda _, body: event({"content": str(has_task(body))})]) as server:
        env = base_env(home, server.url)
        env["UAGENT_DEPTH"] = "1"
        env["UAGENT_SUBAGENT_DEPTH"] = "2"
        env["UAGENT_TOOLSET"] = "lean"
        result = run(root, env, "-p", "probe")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "False", result.stdout)


def test_subagent_reports_the_limit_that_stopped_the_child(root, home):
    """A child that hits a ceiling says which one, so the caller can decide."""

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            # Two calls in one round against a ceiling of one: the child
            # refuses the batch and stops on max_tool_calls.
            return event(
                {
                    "tool_calls": [
                        {
                            "index": i,
                            "id": f"child-call-{i}",
                            "function": {
                                "name": "run",
                                "arguments": json.dumps({"command": "echo child-work"}),
                            },
                        }
                        for i in range(2)
                    ]
                },
                finish="tool_calls",
            )
        results = tool_results(messages)
        if results:
            report = results[-1]
            assert_true("max_tool_calls" in report, report)
            assert_true("child stopped" in report, report)
            # The parent gets the child's answer and reason, not its envelope.
            assert_true("uagent.headless.v1" not in report, report)
            # And a pointer to everything the child printed.
            assert_true("captured log:" in report, report)
            return event({"content": "limit-reported-ok"})
        return tool_call(
            "subagent",
            {"prompt": "child", "background": False, "max_tool_calls": 1},
        )

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "delegate", timeout=30)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "limit-reported-ok", result.stdout)


def test_subagent_answer_survives_a_record_larger_than_the_cap(root, home):
    """A child's record is read whole, however much trace it drags behind it."""

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            if tool_results(messages):
                return event({"content": "child-answer-9f3a"})
            # Enough captured output that the envelope this child prints is
            # far larger than the cap its parent reads a tool result through.
            return tool_call("run", {"command": "printf 'x%.0s' $(seq 1 60000)"})
        results = tool_results(messages)
        if results:
            report = results[-1]
            # The answer, not the slice of trace the cap happened to land on.
            assert_true("child-answer-9f3a" in report, report)
            assert_true("no result envelope" not in report, report)
            assert_true("uagent.headless.v1" not in report, report)
            return event({"content": "recovered-ok"})
        return tool_call("subagent", {"prompt": "child", "background": False})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "delegate", timeout=60)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "recovered-ok", result.stdout)


def test_subagent_foreground_outlives_the_per_call_budget(root, home):
    """A child the caller waits for is bounded by max_seconds, not by the
    budget that stops a runaway command.

    The child spends its time working rather than stalling, so this measures
    the parent's per-call budget and not the child's first-event timeout.
    """

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            child_results = tool_results(messages)
            if any("child-slept" in result for result in child_results):
                return event({"content": "slow-child-result"})
            return tool_call("run", {"command": "sleep 2; echo child-slept"})
        results = tool_results(messages)
        if any("slow-child-result" in result for result in results):
            return event({"content": "slow-child-ok"})
        return tool_call("subagent", {"prompt": "child", "background": False})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_TOOL_TIMEOUT"] = "1"
        result = run(root, env, "--yolo", "-p", "delegate", timeout=40)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "slow-child-ok", result.stdout)


def test_subagent_clamps_are_reported_not_silent(root, home):
    """A background launch and its completion both retain host clamps."""

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            return event({"content": "clamped-child-result"})
        results = tool_results(messages)
        if any("clamped-child-result" in result for result in results):
            report = next(
                result for result in reversed(results) if "clamped-child-result" in result
            )
            assert_true("clamped max_seconds to 2" in report, report)
            return event({"content": "clamp-reported-ok"})
        if any("[started] subagent id " in result for result in results):
            receipt = next(result for result in results if "[started] subagent id " in result)
            assert_true("clamped max_seconds to 2" in receipt, receipt)
            assert_true("produced no result envelope" not in receipt, receipt)
            return tool_call("activity", {"operation": "wait", "wait_ms": 30000})
        return tool_call(
            "subagent",
            {"prompt": "child", "background": True, "max_seconds": 600},
        )

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_SUBAGENT_TIMEOUT"] = "2"
        result = run(root, env, "--yolo", "-p", "delegate", timeout=30)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "clamp-reported-ok", result.stdout)
