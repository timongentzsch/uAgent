import json

from integration_support import (
    Server,
    assert_token_budget_stop,
    assert_true,
    base_env,
    event,
    function_names,
    run,
    tool_call,
    tool_calls,
    tool_results,
    write_session,
)


def test_turn_token_budget_stops_after_one_response_overshoot(root, home, *, binary):
    marker = root / "token-budget-marker"
    response = tool_call("run", {"command": f"touch {marker}"})
    response["usage"] = {
        "prompt_tokens": 10,
        "completion_tokens": 9,
        "completion_tokens_details": {"reasoning_tokens": 2},
    }
    with Server([response]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MAX_TURN_TOKENS"] = "5"
        result = run(root, env, "--yolo", "--json", "-p", "inspect", binary=binary)
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 1, envelope)
        assert_true(envelope["stop"]["reason"] == "turn_tokens", envelope)
        assert_true(envelope["stop"]["generated_tokens"] == 9, envelope)
        assert_true(envelope["stop"]["limits"]["max_turn_tokens"] == 5, envelope)
        assert_true(not marker.exists(), marker)
        assert_true(len(server.requests) == 1, server.requests)


def test_resumed_session_token_budget_stops_before_model_call(root, home, *, binary):
    write_session(
        home,
        "token-budget-resume",
        [{"role": "system", "content": "saved system"}],
        cwd=root,
        kinds=["system"],
        usage={"output": 5},
        session_id="token-budget-resume",
        turns=0,
        title="saved session",
    )
    with Server([event({"content": "too-late"})]) as server:
        envelope = assert_token_budget_stop(
            root, home, server, "-c", "--token-budget", "5", "-p", "continue", binary=binary
        )
        assert_true(envelope["stop"]["session_generated_tokens"] == 5, envelope)
        assert_true(len(server.requests) == 0, server.requests)


def test_session_budget_stops_before_the_next_call(root, home, *, binary):
    expensive = tool_call("read_path", {"path": "."})
    expensive["usage"] = {
        "prompt_tokens": 10,
        "completion_tokens": 2,
        "cost": 0.06,
    }
    with Server([expensive, event({"content": "too-late"})]) as server:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            "--budget",
            "0.05",
            "--json",
            "-p",
            "inspect",
            binary=binary,
        )
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 1, envelope)
        assert_true("session cost limit exceeded" in envelope["error"], envelope)
        assert_true(len(server.requests) == 1, server.requests)


def test_turn_cost_is_unlimited_by_default(root, home, *, binary):
    expensive = tool_call("read_path", {"path": "."})
    expensive["usage"] = {
        "prompt_tokens": 10,
        "completion_tokens": 2,
        "cost": 1.50,
    }
    with Server([expensive, event({"content": "cost-unlimited-ok"})]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "inspect", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("cost-unlimited-ok"), result.stdout)
        assert_true(len(server.requests) == 2, server.requests)


def test_tool_policy_scopes_schema_and_runtime(root, home, *, binary):
    marker = root / "tool-policy-marker"

    def request_forbidden(_, body):
        names = function_names(body)
        if names != {"grep", "read_path", "run"}:
            return event({"content": f"bad-schema:{sorted(names)}"})
        return tool_call("run", {"command": f"touch {marker}"})

    def verify_rejected(_, body):
        results = tool_results(body["messages"])
        rejected = any("not allowed by tool policy" in result for result in results)
        return event({"content": "policy-ok" if rejected else "policy-bad"})

    with Server([request_forbidden, verify_rejected]) as server:
        env = base_env(home, server.url)
        env.update(
            {
                "UAGENT_TOOL_CAPABILITIES": "inspect",
                "UAGENT_INTERNAL_TOOL_ALLOWLIST": json.dumps(["grep", "read_path", "run"]),
                "UAGENT_INTERNAL_TOOL_RUN_ALLOWLIST": json.dumps(["python3 slow_analysis.py"]),
                "PYTHONDONTWRITEBYTECODE": "1",
            }
        )
        result = run(root, env, "--yolo", "-p", "inspect only", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "policy-ok", result.stdout)
        assert_true(not marker.exists(), marker)


def test_tool_call_budget_is_unlimited_by_default(root, home, *, binary):
    source = root / "many-lines.txt"
    source.write_text("\n".join(str(i) for i in range(101)), encoding="utf-8")
    calls = [
        (f"read-{i}", "read_path", {"path": str(source), "offset": i + 1, "limit": 1})
        for i in range(101)
    ]

    def finish(_, body):
        count = len(tool_results(body["messages"]))
        return event({"content": "unlimited-tools-ok" if count == 101 else f"only-{count}"})

    with Server([tool_calls(calls), finish]) as server:
        env = base_env(home, server.url)
        env["UAGENT_AUTO_COMPACT_PCT"] = "0"
        result = run(root, env, "--yolo", "-p", "read every line", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("unlimited-tools-ok"), result.stdout)
