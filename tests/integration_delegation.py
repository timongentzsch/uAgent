from integration_support import (
    BINARY,
    Server,
    assert_true,
    base_env,
    descendant_pids,
    event,
    function_names,
    function_tool,
    has_message,
    json,
    run,
    run_dialog,
    signal,
    subprocess,
    threading,
    time,
    tool_call,
    tool_results,
    wait_for_processes_stopped,
    wait_until,
    write_http_response,
)


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
        task = function_tool(body, "subagent")
        background = task["parameters"]["properties"]["background"]
        assert_true(background["type"] == "boolean", background)
        assert_true("final result directly" in background["description"], background)
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
        lifecycle = {"get_task_output", "wait_tasks", "kill_task"}
        assert_true(3 <= len(parent_requests) <= 4, len(parent_requests))
        for request in parent_requests:
            names = function_names(request)
            assert_true("subagent" in names, names)
            assert_true(not names.intersection(lifecycle), names)
            task_schema = function_tool(request, "subagent")
            assert_true(
                {"prompt", "model"}.issubset(task_schema["parameters"]["properties"]),
                task_schema,
            )


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
        task = function_tool(body, "subagent")
        assert_true("provider" not in task["parameters"]["properties"], task)
        description = task["parameters"]["properties"]["model"]["description"]
        assert_true("codex-local/MODEL" in description, description)
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
    """Loosening past a host ceiling is clamped, and the caller is told."""

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            return event({"content": "clamped-child-result"})
        results = tool_results(messages)
        if results:
            report = results[-1]
            assert_true("clamped max_seconds to 2" in report, report)
            return event({"content": "clamp-reported-ok"})
        return tool_call(
            "subagent",
            {"prompt": "child", "background": False, "max_seconds": 600},
        )

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_SUBAGENT_TIMEOUT"] = "2"
        result = run(root, env, "--yolo", "-p", "delegate", timeout=30)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "clamp-reported-ok", result.stdout)


TESTS = (
    test_subagent_auto_join_continues_turn,
    test_subagent_reports_the_limit_that_stopped_the_child,
    test_subagent_foreground_outlives_the_per_call_budget,
    test_subagent_clamps_are_reported_not_silent,
    test_subagent_foreground_returns_result_without_wait_round,
    test_parallel_subagents_auto_join,
    test_subagent_interrupt_reaps_child,
    test_subagent_uses_selected_model_route,
    test_subagent_failure_reports_route_stage_and_bounded_diagnostics,
    test_subagent_recursion_is_depth_bounded,
)
