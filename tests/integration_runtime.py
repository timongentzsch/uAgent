import stat

from integration_support import (
    SMALL_PNG,
    Server,
    assert_true,
    base_env,
    budget,
    event,
    function_names,
    function_tool,
    has_message,
    json,
    midturn_compaction_env,
    os,
    pathlib,
    re,
    run,
    run_dialog,
    run_pty,
    shlex,
    sse,
    threading,
    time,
    tool_call,
    tool_calls,
    tool_results,
    wait_until,
    write_json_response,
    write_session,
)
from memory_fixture import global_memory_dir, project_memory_dir


def test_plain_turn(root, home):
    def reply(_, body):
        names = function_names(body)
        assert_true("activity" not in names, names)
        return event({"content": "ok"}, usage={"prompt_tokens": 2, "completion_tokens": 1})

    with Server([reply]) as server:
        result = run(root, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "ok", result.stdout)


def test_adaptive_system_revises_replaces_and_clears(root, home):
    def initial(_, body):
        assert_true("adapt_system" in function_names(body), function_names(body))
        schema = function_tool(body, "adapt_system")["parameters"]
        assert_true(set(schema["required"]) == {"instructions", "reason"}, schema)
        assert_true("MUTABLE SELF-DIRECTIVE" not in body["messages"][0]["content"], body)
        return tool_call(
            "adapt_system",
            {
                "instructions": "Inspect broadly and challenge the initial hypothesis.",
                "reason": "The task is still ambiguous.",
            },
            call_id="adapt-1",
        )

    def replace(_, body):
        system = body["messages"][0]["content"]
        assert_true("MUTABLE SELF-DIRECTIVE revision 1" in system, system)
        assert_true("Inspect broadly and challenge" in system, system)
        assert_true(system.rfind("[HOST CAPABILITIES]") > system.rfind("[END MUTABLE"), system)
        return tool_call(
            "adapt_system",
            {
                "instructions": "Stop broad exploration and validate the localized invariant.",
                "reason": "New evidence localized the issue.",
            },
            call_id="adapt-2",
        )

    def clear(_, body):
        system = body["messages"][0]["content"]
        assert_true("MUTABLE SELF-DIRECTIVE revision 2" in system, system)
        assert_true("validate the localized invariant" in system, system)
        assert_true("Inspect broadly and challenge" not in system, system)
        return tool_call(
            "adapt_system",
            {"instructions": "", "reason": "Specialized execution is complete."},
            call_id="adapt-3",
        )

    def finish(_, body):
        system = body["messages"][0]["content"]
        assert_true("MUTABLE SELF-DIRECTIVE" not in system, system)
        results = tool_results(body["messages"])
        assert_true(any("revision 3 cleared" in result for result in results), results)
        return event({"content": "adaptive-system-ok"})

    with Server([initial, replace, clear, finish]) as server:
        trace = root / "adaptive-system.jsonl"
        env = base_env(home, server.url)
        env["UAGENT_ADAPT_SYSTEM"] = "1"
        result = run(root, env, "--yolo", f"--debug={trace}", "-p", "adapt as needed")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("adaptive-system-ok"), result.stdout)
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        revisions = [
            record["data"]["revision"]
            for record in records
            if record.get("event") == "system_adapted"
        ]
        assert_true(revisions == [1, 2, 3], revisions)
        snapshots = [record["data"] for record in records if record.get("event") == "model_request"]
        assert_true([item["system_revision"] for item in snapshots] == [0, 1, 2, 3], snapshots)

    def static_reply(_, body):
        assert_true("adapt_system" not in function_names(body), function_names(body))
        return event({"content": "static-system-ok"})

    with Server([static_reply]) as server:
        result = run(root, base_env(home, server.url), "-p", "work normally")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "static-system-ok", result.stdout)


def test_stream_error_is_not_an_empty_response(root, home):
    with Server([{"error": {"message": "upstream overloaded", "type": "server_error"}}]) as server:
        result = run(root, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode != 0, result.stdout)
        assert_true("upstream overloaded" in result.stderr, result.stderr)
        assert_true("empty response" not in result.stderr, result.stderr)
        assert_true(len(server.requests) == 3, server.requests)


def test_empty_response_after_tools_recovers(root, home):
    def recovered(_, body):
        contents = [
            message.get("content", "")
            for message in body["messages"]
            if message.get("role") == "user"
        ]
        valid = any(
            "Return the final answer from existing results" in str(content) for content in contents
        )
        return event({"content": "recovered" if valid else "missing-recovery"})

    def unchanged(_, body):
        # The first barren completion is replayed as it was: reacting to a
        # provider hiccup would mutate the history for nothing.
        contents = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "user"
        ]
        assert_true(not any("empty model response" in content for content in contents), contents)
        return event()

    with Server(
        [
            tool_call("read_path", {"path": "."}),
            event(),
            unchanged,
            recovered,
        ]
    ) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "inspect")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "recovered", result.stdout)
        assert_true(len(server.requests) == 4, len(server.requests))


def test_foreign_tool_markup_recovers_as_prose(root, home):
    markup = '<｜DSML｜tool_calls:\n    edit_file:\n      path: path="test.cc"'

    def recovered(_, body):
        notes = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "user"
        ]
        assert_true(any("invalid model tool markup" in note for note in notes), notes)
        return event({"content": "markup-recovered"})

    with Server([event({"content": markup}), recovered]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "answer")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "markup-recovered", result.stdout)

    with Server([event({"content": markup}), event({"content": markup})]) as repeated:
        result = run(root, base_env(home, repeated.url), "--yolo", "-p", "answer")
        assert_true(result.returncode != 0, result.stdout)
        assert_true("repeatedly returned invalid tool markup" in result.stderr, result.stderr)
        assert_true(len(repeated.requests) == 2, len(repeated.requests))

    # The compatibility text protocol is executable only after this route has
    # explicitly rejected native tools. In native mode it is malformed prose,
    # even if its arguments would otherwise be valid and auto-approved.
    marker = home / "native-text-protocol-must-not-run"
    own_markup = (
        "[uagent_tool_call]"
        + json.dumps({"name": "run", "arguments": {"command": f"touch {marker}"}})
        + "[/uagent_tool_call]"
    )

    def native_recovered(_, body):
        assert_true("tools" in body, body)
        notes = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "user"
        ]
        assert_true(any("invalid model tool markup" in note for note in notes), notes)
        return event({"content": "native-markup-recovered"})

    with Server([event({"content": own_markup}), native_recovered]) as native:
        result = run(root, base_env(home, native.url), "--yolo", "-p", "answer")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "native-markup-recovered", result.stdout)
        assert_true(not marker.exists(), marker)

    with Server(
        [
            tool_call("read_path", {"path": "."}),
            event(),
            event(),
            event(),
        ]
    ) as exhausted:
        result = run(root, base_env(home, exhausted.url), "--yolo", "-p", "inspect")
        assert_true(result.returncode != 0, result.stdout)
        assert_true("model returned an empty response" in result.stderr, result.stderr)
        assert_true(len(exhausted.requests) == 4, len(exhausted.requests))


def test_transient_stream_errors_retry_before_progress(root, home):
    with Server(
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
    ) as server:
        result = run(root, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "retry-ok", result.stdout)
        assert_true(len(server.requests) == 3, server.requests)


def test_config_reload_applies_only_at_turn_boundaries(root, home):
    config = home / ".uagent" / ".config"
    config.parent.mkdir(parents=True)
    config.write_text("UAGENT_MAX_TOOL_CALLS=2\n", encoding="utf-8")

    def two_calls():
        return event(
            {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "one",
                        "function": {
                            "name": "read_path",
                            "arguments": json.dumps({"path": "."}),
                        },
                    },
                    {
                        "index": 1,
                        "id": "two",
                        "function": {
                            "name": "read_path",
                            "arguments": json.dumps({"path": "."}),
                        },
                    },
                ]
            },
            finish="tool_calls",
        )

    def change_during_first_request(_, __):
        config.write_text("UAGENT_MAX_TOOL_CALLS=1\n", encoding="utf-8")
        return two_calls()

    with Server(
        [
            change_during_first_request,
            event({"content": "first-turn-used-snapshot"}),
            two_calls(),
        ]
    ) as server:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "first\nsecond\n/q\n",
            timeout=12,
        )
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true("first-turn-used-snapshot" in result.stdout, result.stdout)
        assert_true("configuration reloaded for the next turn" in result.stdout, result.stdout)
        assert_true("tool call limit reached (1)" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 3, server.requests)


def test_prompt_overlay_replaces_base_sections(root, home):
    """The experiment overlay may reword the base prompt and nothing else.

    Rebuilding to compare two prompt variants is what stops prompt work from
    being measured, so the swap is a file; authority stays with the host.
    """
    workspace = root / "overlay-workspace"
    workspace.mkdir(parents=True)
    overlay = workspace / "overlay.json"
    overlay.write_text(
        json.dumps(
            {
                "replace": {"## Answer": "OVERLAY-ANSWER-RULE"},
                "append": "OVERLAY-TAIL",
            }
        ),
        encoding="utf-8",
    )

    def verify(_, body):
        prompt = body["messages"][0].get("content", "")
        valid = (
            "OVERLAY-ANSWER-RULE" in prompt
            and "OVERLAY-TAIL" in prompt
            # The replaced section is gone, its neighbours are intact, and the
            # host-owned sections still follow the overlaid base.
            and "Cite code as path:line" not in prompt
            and "## Delegation" in prompt
            and "Inquiries do not authorize" in prompt
            and "[HOST CAPABILITIES]" in prompt
            and prompt.index("OVERLAY-TAIL") < prompt.index("[HOST CAPABILITIES]")
        )
        return event({"content": "overlay-ok" if valid else f"overlay-bad: {prompt[:400]}"})

    with Server([verify]) as server:
        env = base_env(home, server.url)
        env["UAGENT_PROMPT_OVERLAY"] = str(overlay)
        result = run(workspace, env, "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "overlay-ok", result.stdout)

    # An unreadable or malformed overlay is ignored rather than fatal: an
    # experiment must not be able to break a session.
    def baseline(_, body):
        prompt = body["messages"][0].get("content", "")
        return event({"content": "base-ok" if "Cite code as path:line" in prompt else "base-bad"})

    overlay.write_text("{not json", encoding="utf-8")
    with Server([baseline]) as server:
        env = base_env(home, server.url)
        env["UAGENT_PROMPT_OVERLAY"] = str(overlay)
        result = run(workspace, env, "-p", "reply")
        assert_true(result.stdout.strip() == "base-ok", result.stdout)


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
        # Project instructions ride inside the single baseline system message.
        instructions = messages[0].get("content", "") if messages else ""
        users = [str(m.get("content", "")) for m in messages if m.get("role") == "user"]
        valid = (
            messages[0].get("role") == "system"
            and not any(m.get("role") == "system" for m in messages[1:])
            and any(text.startswith("[environment:") for text in users)
            and "reply" in users
            and "<INSTRUCTIONS>" in instructions
            and "shadowed-claude-sentinel" not in instructions
            and instructions.index("root-agent-sentinel")
            < instructions.index("nested-agent-sentinel")
        )
        return event({"content": "instructions-ok" if valid else "instructions-bad"})

    with Server([verify]) as server:
        result = run(nested, base_env(home, server.url), "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "instructions-ok", result.stdout)


def test_session_title_replaces_initial_greeting(root, home):
    with Server([event({"content": "hello-ok"}), event({"content": "task-ok"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"hello\n", b"hello-ok"),
                b"",
                (b"investigate browser efficiency\n", b"task-ok"),
                b"",
                b"/q\n",
            ],
        )
        assert_true(code == 0, output)
        assert_true(b"task-ok" in output, output)
        assert_true(len(server.requests) == 2, server.requests)
        sessions = list((home / ".uagent" / "history").rglob("*.json"))
        assert_true(len(sessions) == 1, sessions)
        header = json.loads(sessions[0].read_text(encoding="utf-8").splitlines()[0])
        assert_true(header["title"] == "investigate browser efficiency", header)
        journal = pathlib.Path(str(sessions[0]) + ".events.jsonl")
        assert_true(journal.exists(), journal)
        assert_true(stat.S_IMODE(journal.stat().st_mode) == 0o600, journal.stat())
        records = [json.loads(line) for line in journal.read_text().splitlines()]
        types = [record["type"] for record in records]
        assert_true(types[0] == "session.ready", types)
        assert_true(types[-1] == "session.ended", types)
        assert_true(types.count("turn.started") == 2, types)
        assert_true(types.count("turn.completed") == 2, types)
        assert_true("investigate browser efficiency" not in journal.read_text(), records)


def test_input_steering_yields_activity_wait(root, home):
    def route(_, body):
        messages = body["messages"]
        results = tool_results(messages)
        if has_message(messages, "user", "change course"):
            yielded = any("wait yielded for queued steering" in result for result in results)
            still_running = any("activity(s) still running" in result for result in results)
            return event(
                {
                    "content": (
                        "steering-wait-ok" if yielded and still_running else "steering-wait-bad"
                    )
                }
            )
        if any("[running] activity" in result for result in results):
            return tool_call("activity", {"operation": "wait", "wait_ms": 30000})
        return tool_call("run", {"command": "sleep 30", "yield_ms": 250})

    with Server([route]) as server:
        started = time.monotonic()
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                # Wait for the activity tool CALL, not the substring: the
                # startup banner lists "activity" and the run result says
                # "[running] activity <id>", so a bare marker fires before the
                # model has issued the wait and steering lands a round early.
                (b"start\n", b"activity(wait"),
                (b"change course\n", b"steering-wait-ok"),
                b"/q\n",
            ],
            args=("--yolo",),
            timeout=8,
        )
        elapsed = time.monotonic() - started
        assert_true(code == 0, output)
        assert_true(b"steering-wait-ok" in output, output)
        assert_true(b"steering-wait-bad" not in output, output)
        assert_true(elapsed < 8, elapsed)


def test_input_idle_background_completion_is_observational(root, home):
    def route(_, body):
        messages = body["messages"]
        results = tool_results(messages)
        if any(
            message.get("role") == "user" and message.get("content") == "next"
            for message in messages
        ):
            leaked = any(
                "[Background result:" in str(message.get("content", "")) for message in messages
            )
            return event({"content": "background-leaked" if leaked else "background-ui-only"})
        if any("[running] activity" in result for result in results):
            return event({"content": "background-launched"})
        return tool_call("run", {"command": "sleep 0.8; printf notified", "yield_ms": 250})

    with Server([route]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"start\n", b"background-launched"),
                (b"", b"bg job finished"),
                (b"next\n", b"background-ui-only"),
                b"/q\n",
            ],
            args=("--yolo",),
            timeout=8,
        )
        assert_true(code == 0, output)
        assert_true(b"notified" in output, output)
        assert_true(b"background-ui-only" in output, output)
        assert_true(b"background-leaked" not in output, output)
        assert_true(len(server.requests) == 3, server.requests)


def test_headless_json_envelope_contains_trace_usage_and_exit(root, home):
    first = tool_call("run", {"command": "printf tool-json"})
    first["usage"] = {
        "prompt_tokens": 7,
        "completion_tokens": 3,
        "completion_tokens_details": {"reasoning_tokens": 1},
        "cost": 0.01,
    }
    with Server(
        [
            first,
            event(
                {"content": "final-json-answer"},
                usage={"prompt_tokens": 5, "completion_tokens": 2, "cost": 0.02},
            ),
        ]
    ) as server:
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


def test_headless_json_stream_emits_lifecycle_events(root, home):
    with Server(
        [
            tool_call(
                "activity",
                {
                    "operation": "resize",
                    "id": 2147483000,
                    "rows": 0,
                    "cols": 997,
                },
            ),
            event(
                {"content": "stream-answer"},
                usage={"prompt_tokens": 4, "completion_tokens": 2, "cost": 0.01},
            ),
        ]
    ) as server:
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
        assert_true(all(item["schema"] == "uagent.event.v2" for item in records), records)
        types = [item["type"] for item in records]
        assert_true(types[0] == "turn.started", types)
        assert_true("tool.call" in types and "tool.result" in types, types)
        tool_event = next(item for item in records if item["type"] == "tool.call")
        metadata = tool_event["data"]
        assert_true("arguments" not in metadata and "parsed_arguments" not in metadata, metadata)
        assert_true(metadata["argument_keys"] == ["cols", "id", "operation", "rows"], metadata)
        assert_true(
            metadata["argument_types"]
            == {"cols": "number", "id": "number", "operation": "string", "rows": "number"},
            metadata,
        )
        assert_true(metadata["operation"] == "resize", metadata)
        assert_true(metadata["issue_code"] == "activity.invalid_dimensions", metadata)
        assert_true(metadata["issue_field"] == "", metadata)
        result_event = next(item for item in records if item["type"] == "tool.result")
        assert_true(
            result_event["data"]["issue_code"] == "activity.invalid_dimensions", result_event
        )
        assert_true(result_event["data"]["issue_field"] == "", result_event)
        encoded_metadata = json.dumps(metadata)
        assert_true(
            "2147483000" not in encoded_metadata and "997" not in encoded_metadata, metadata
        )
        assert_true("usage" in types and types[-1] == "answer", types)
        assert_true(records[-1]["data"]["answer"] == "stream-answer", records[-1])


def test_session_budget_stops_before_the_next_call(root, home):
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
        )
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 1, envelope)
        assert_true("session cost limit exceeded" in envelope["error"], envelope)
        assert_true(len(server.requests) == 1, server.requests)


def test_turn_cost_is_unlimited_by_default(root, home):
    expensive = tool_call("read_path", {"path": "."})
    expensive["usage"] = {
        "prompt_tokens": 10,
        "completion_tokens": 2,
        "cost": 1.50,
    }
    with Server([expensive, event({"content": "cost-unlimited-ok"})]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "inspect")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("cost-unlimited-ok"), result.stdout)
        assert_true(len(server.requests) == 2, server.requests)


def test_tool_policy_scopes_schema_and_runtime(root, home):
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
                "UAGENT_TOOL_ALLOWLIST": json.dumps(["grep", "read_path", "run"]),
                "UAGENT_TOOL_RUN_ALLOWLIST": json.dumps(["python3 slow_analysis.py"]),
                "PYTHONDONTWRITEBYTECODE": "1",
            }
        )
        result = run(root, env, "--yolo", "-p", "inspect only")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "policy-ok", result.stdout)
        assert_true(not marker.exists(), marker)


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

    def memory_context(body):
        messages = body["messages"]
        marker = "[memory names only; non-authoritative metadata]"
        system = str(messages[0].get("content", "")) if messages else ""
        memories = system[system.index(marker) :] if marker in system else ""
        return messages, memories

    def verify(_, body):
        messages, memories = memory_context(body)
        valid = (
            bool(memories)
            and "global/style" in memories
            and "[always-on behavioral memory; non-authoritative evidence]" in memories
            and "global-memory-sentinel" in memories
            and "project/build" in memories
            and "project-memory-sentinel" not in memories
            and messages[0].get("role") == "system"
        )
        return event({"content": "memory-ok" if valid else "memory-bad"})

    def verify_isolated(_, body):
        _, memories = memory_context(body)
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


def test_configured_redaction_keywords_apply(root, home):
    # The keyword list is read once per process, so this needs a fresh agent
    # rather than a unit test. Configured keywords must extend the built-ins,
    # never replace them, and must be matched literally.
    workspace = root / "redact-workspace"
    workspace.mkdir()
    # Global memories carry their body into the system prompt; project ones are
    # listed by name only, so only a global memory exercises the redactor here.
    global_dir = global_memory_dir(home)
    global_dir.mkdir(parents=True, exist_ok=True)
    (global_dir / "creds.md").write_text(
        "db_dsn=postgres://user/redact-dsn-sentinel\n"
        "passwd=redact-passwd-sentinel\n"
        "harmless value 42\n",
        encoding="utf-8",
    )

    def verify(_, body):
        system = str(body["messages"][0].get("content", ""))
        valid = (
            "redact-dsn-sentinel" not in system
            and "redact-passwd-sentinel" not in system
            and "[REDACTED]" in system
            and "harmless value 42" in system
        )
        return event({"content": "redact-ok" if valid else "redact-bad"})

    server = Server([verify])
    try:
        env = base_env(home, server.url)
        # ".*" must be treated as a literal keyword, not a pattern.
        env["UAGENT_MEMORY_REDACT_KEYWORDS"] = "db_dsn, .*"
        result = run(workspace, env, "-p", "reply")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "redact-ok", result.stdout)
    finally:
        server.close()
        # HOME is shared by every test; a global memory would join them all.
        (global_dir / "creds.md").unlink(missing_ok=True)


def test_memory_background_extractor_is_bounded(root, home):
    workspace = root / "memory-extract-workspace"
    workspace.mkdir()
    session = write_session(
        home,
        "extract",
        [
            {"role": "system", "content": "system-content-must-not-leak"},
            {"role": "user", "content": "memory-extract-user-sentinel"},
            {"role": "assistant", "content": "first answer"},
            {"role": "user", "content": "please keep fixes concise"},
            {"role": "assistant", "content": "understood"},
        ],
        cwd=workspace,
        session_id="memory-extract-test",
        turns=2,
        title="durable preference",
    )
    target = project_memory_dir(home, workspace) / "extracted.md"

    def extract(_, body):
        text = json.dumps(body)
        valid = (
            function_names(body) == {"memory"}
            and "memory-extract-user-sentinel" in text
            and "system-content-must-not-leak" not in text
            and "at most one durable" in text
        )
        if not valid:
            return event({"content": "extract-schema-bad"})
        return tool_call("memory", {"action": "list"})

    def search(_, _body):
        return tool_call("memory", {"action": "search", "key": "concise"})

    def inspect(_, _body):
        return tool_call("memory", {"action": "get", "key": "project/extracted"})

    def write(_, _body):
        return tool_call(
            "memory",
            {
                "action": "set",
                "key": "project/extracted",
                "content": "Keep repository fixes concise.",
            },
        )

    def finish(_, body):
        wrote = any(
            message.get("role") == "tool" and "wrote " in str(message.get("content", ""))
            for message in body["messages"]
        )
        return event({"content": "extract-done" if wrote else "extract-write-bad"})

    with Server([extract, search, inspect, write, finish]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MEMORY_IDLE_SECONDS"] = "0"

        def extracted():
            markers = list((home / ".uagent/memory/.processed").rglob("*.state"))
            return target.exists() and any(
                marker.read_text(encoding="utf-8").strip() == "done" for marker in markers
            )

        code, output = run_pty(
            workspace,
            env,
            b"/q\n",
            before_payload=lambda: wait_until(
                extracted, "background memory extraction did not finish", timeout=5
            ),
        )
        assert_true(code == 0, output)
        assert_true(target.read_text(encoding="utf-8") == "Keep repository fixes concise.", target)
        assert_true(len(server.requests) == 5, server.requests)
        assert_true(b"memory-extract-user-sentinel" not in output, output)
        assert_true(b"Background result" not in output, output)

        # A completed source is not processed again. Disabling generation also
        # prevents a changed source from becoming eligible.
        code, output = run_pty(workspace, env, b"/q\n", before_payload=lambda: time.sleep(0.2))
        assert_true(code == 0 and len(server.requests) == 5, output)
        time.sleep(0.01)
        os.utime(session, None)
        disabled = dict(env)
        disabled["UAGENT_MEMORY_GENERATE"] = "0"
        code, output = run_pty(
            workspace,
            disabled,
            b"/q\n",
            before_payload=lambda: time.sleep(0.2),
        )
        assert_true(code == 0 and len(server.requests) == 5, output)


def test_memory_background_extractor_releases_failed_claims(root, _home):
    def scenario(name, kinds=None):
        case_home = root / f"memory-{name}-home"
        workspace = root / f"memory-{name}-workspace"
        workspace.mkdir()
        write_session(
            case_home,
            "extract",
            [
                {"role": "user", "content": f"remember-{name}"},
                {"role": "assistant", "content": "understood"},
            ],
            cwd=workspace,
            kinds=kinds,
            session_id=f"memory-{name}",
            turns=2,
            title=name,
        )
        return case_home, workspace

    def markers(case_home):
        return list((case_home / ".uagent/memory/.processed").rglob("*.state"))

    no_write_home, no_write_workspace = scenario("no-write")
    with Server([event({"content": "Nothing durable to save."})]) as server:
        env = base_env(no_write_home, server.url)
        env["UAGENT_MEMORY_IDLE_SECONDS"] = "0"

        def wait_for_done():
            wait_until(
                lambda: any(
                    marker.read_text(encoding="utf-8").strip() == "done"
                    for marker in markers(no_write_home)
                ),
                "successful no-write extraction did not finish",
            )

        code, output = run_pty(
            no_write_workspace,
            env,
            b"/q\n",
            before_payload=wait_for_done,
        )
        assert_true(code == 0, output)
        assert_true(len(server.requests) == 1, server.requests)
        assert_true(not list((no_write_home / ".uagent/memory").rglob("*.md")), no_write_home)

    def run_cleanup_case(name, responder, kinds=None, wait_for_request=False):
        case_home, workspace = scenario(name, kinds)
        trace = root / f"memory-{name}.jsonl"
        with Server([responder]) as server:
            env = base_env(case_home, server.url)
            env["UAGENT_MEMORY_IDLE_SECONDS"] = "0"
            env["UAGENT_REQUEST_TIMEOUT"] = "1"
            env["UAGENT_FIRST_EVENT_TIMEOUT"] = "1"
            env["UAGENT_STREAM_IDLE_TIMEOUT"] = "1"

            def completion_logged():
                return trace.exists() and '"event":"memory_extract_finished"' in trace.read_text(
                    encoding="utf-8"
                )

            def wait_for_cleanup():
                if wait_for_request:
                    wait_until(lambda: bool(server.requests), f"{name} request did not start")
                if name == "terminated":
                    wait_until(
                        lambda: any(
                            marker.read_text(encoding="utf-8").strip() == "processing"
                            for marker in markers(case_home)
                        ),
                        "termination case did not retain its live claim",
                    )
                else:
                    wait_until(completion_logged, f"{name} extractor did not complete")

            code, output = run_pty(
                workspace,
                env,
                b"/q\n",
                args=(f"--debug={trace}",),
                before_payload=wait_for_cleanup,
                timeout=40,
            )
            assert_true(code == 0, output)
            if name != "terminated":
                assert_true(completion_logged(), f"{name} extractor did not complete")
            wait_until(lambda: not markers(case_home), f"{name} claim survived shutdown")
            return server.requests

    def fail_request(handler, _):
        # This case owns claim cleanup, not transient-retry timing. A terminal
        # model error keeps the completion bound deterministic under ASan.
        write_json_response(handler, {"error": {"message": "extract failed"}}, status=400)

    failed_requests = run_cleanup_case("model-failure", fail_request, wait_for_request=True)
    assert_true(failed_requests, "model failure did not reach the provider")

    # One kind for two messages: a session the agent would never write, which
    # must be rejected before it reaches the provider.
    invalid_requests = run_cleanup_case(
        "invalid-session", event({"content": "unused"}), kinds=["user"]
    )
    assert_true(not invalid_requests, invalid_requests)

    request_started = threading.Event()

    def block_request(_, __):
        request_started.set()
        time.sleep(20)
        return event({"content": "too late"})

    run_cleanup_case("terminated", block_request, wait_for_request=True)
    assert_true(request_started.is_set(), "termination case never entered the provider request")


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

    with Server([stall, stall, stall]) as server:
        env = base_env(home, server.url)
        env["UAGENT_FIRST_EVENT_TIMEOUT"] = "1"
        started = time.monotonic()
        result = run(root, env, "-p", "probe")
        elapsed = time.monotonic() - started
        assert_true(result.returncode == 1, result.returncode)
        assert_true("no event within 1s" in result.stderr, result.stderr)
        assert_true(len(server.requests) == 3, server.requests)
        assert_true(3.0 < elapsed < 6.5, elapsed)


def test_midturn_compaction_preserves_progress_and_usage(root, home):
    trace = root / "midturn-compact.jsonl"
    source = root / "midturn-source.txt"
    source.write_text("RAW-TOOL-RESULT-" + "x" * 7800, encoding="utf-8")
    output = root / "midturn-output.txt"

    first = tool_call(
        "read_path",
        {"path": str(source)},
        call_id="midturn-call",
        usage={"prompt_tokens": 2000, "completion_tokens": 10},
    )

    def compact(_, body):
        prompt = body["messages"][-1].get("content", "")
        text = "\n".join(str(message.get("content", "")) for message in body["messages"])
        valid = (
            str(prompt).startswith("Summarize the bounded transcript")
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
            and "Summarize the bounded transcript" not in text
            and "RAW-TOOL-RESULT-" not in text
            and not any(message.get("role") == "tool" for message in messages)
            and not any(message.get("tool_calls") for message in messages)
            and bool(body.get("tools"))
            and any(
                message.get("role") == "user"
                and "MIDTURN-SUMMARY" in str(message.get("content", ""))
                for message in messages
            )
            and any(
                message.get("role") == "user" and message.get("content") == "inspect"
                for message in messages
            )
            and sum(message.get("content") == "inspect" for message in messages) == 1
            and any(
                message.get("role") == "user"
                and str(message.get("content", "")).startswith("[environment:")
                for message in messages
            )
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

    with Server([first, compact, finish, final]) as server:
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


def test_absolute_compaction_ceiling(root, home):
    write_session(
        home,
        "absolute",
        [
            {"role": "system", "content": "saved system"},
            {"role": "user", "content": "prior task"},
            {"role": "assistant", "content": "large " + "x" * 10000},
        ],
        cwd=root,
        context_tokens=2500,
        session_id="absolute-compact",
        turns=1,
        title="prior task",
    )

    def compact(_, body):
        prompt = body["messages"][-1].get("content", "")
        valid = str(prompt).startswith("Summarize the bounded transcript") and not body.get("tools")
        return event({"content": "ABSOLUTE-SUMMARY" if valid else "BAD-SUMMARY"})

    def finish(_, body):
        text = "\n".join(str(message.get("content", "")) for message in body["messages"])
        valid = (
            "Prior context:\nABSOLUTE-SUMMARY" in text
            and "prior task" in text
            and text.count("continue") == 1
            and "large " not in text
        )
        return event({"content": "absolute-compact-ok" if valid else "absolute-compact-bad"})

    with Server([compact, finish]) as server:
        trace = root / "absolute-compact.jsonl"
        env = base_env(home, server.url)
        env.update(
            {
                "UAGENT_CONTEXT": "1000000",
                "UAGENT_MAX_TOKENS": "512",
                "UAGENT_AUTO_COMPACT_PCT": "0",
                "UAGENT_AUTO_COMPACT_TOKENS": "2000",
            }
        )
        result = run(root, env, "-c", f"--debug={trace}", "-p", "continue")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("absolute-compact-ok"), result.stdout)
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        compacted = [record for record in records if record["event"] == "auto_compact"]
        assert_true(len(compacted) == 1, compacted)
        assert_true(compacted[0]["data"]["projected_tokens"] >= 2000, compacted)


def test_activity_progress_polls_do_not_trip_identical_call_guard(root, home):
    state = {"requests": 0, "id": 0}
    trigger_prefix = root / "poll-trigger"
    ack_prefix = root / "poll-ack"
    command = "\n".join(
        [
            f"while [ ! -f {shlex.quote(str(trigger_prefix))}-{i} ]; do sleep 0.01; done; "
            "yes x | head -c 70000; "
            f"printf '\\nprogress-{i}\\n'; : > {shlex.quote(str(ack_prefix))}-{i}"
            for i in range(1, 5)
        ]
        + ["sleep 30"]
    )

    def route(_, body):
        state["requests"] += 1
        results = tool_results(body["messages"])
        running = next((text for text in results if text.startswith("[running] activity ")), "")
        if not running:
            return tool_call("run", {"command": command, "yield_ms": 250})
        match = re.search(r"activity (\d+)", running)
        assert_true(match is not None, running)
        state["id"] = int(match.group(1))
        poll = state["requests"] - 1
        if poll <= 4:
            pathlib.Path(f"{trigger_prefix}-{poll}").touch()
            ack = pathlib.Path(f"{ack_prefix}-{poll}")
            deadline = time.monotonic() + budget(3)
            while not ack.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            assert_true(ack.exists(), f"activity did not produce progress {poll}")
            return tool_call("activity", {"operation": "poll", "id": state["id"]})
        complete = all(
            any(f"progress-{index}" in result for result in results) for index in range(1, 5)
        )
        return event({"content": "progress-polls-ok" if complete else "progress-polls-bad"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_AUTO_COMPACT_PCT"] = "0"
        env["UAGENT_AUTO_COMPACT_TOKENS"] = "0"
        result = run(root, env, "--yolo", "-p", "monitor", timeout=12)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("progress-polls-ok"), result.stdout)
        assert_true(len(server.requests) == 6, len(server.requests))


def test_activity_no_change_polls_are_steered_then_stopped(root, home):
    state = {"requests": 0, "id": 0}

    def route(_, body):
        state["requests"] += 1
        results = tool_results(body["messages"])
        running = next((text for text in results if text.startswith("[running] activity ")), "")
        if not running:
            return tool_call("run", {"command": "sleep 30", "yield_ms": 250})
        match = re.search(r"activity (\d+)", running)
        assert_true(match is not None, running)
        state["id"] = int(match.group(1))
        if state["requests"] == 4:
            combined = "\n".join(str(message.get("content", "")) for message in body["messages"])
            assert_true("[activity poll advisory]" in combined, combined)
        if state["requests"] <= 4:
            return tool_call("activity", {"operation": "poll", "id": state["id"]})
        return event({"content": "fifth-round-should-not-run"})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "monitor", timeout=8)
        assert_true(result.returncode != 0, result.stdout)
        assert_true(
            f"activity {state['id']} produced no new output across 3 consecutive polls"
            in result.stderr,
            result.stderr,
        )
        assert_true(len(server.requests) == 4, len(server.requests))


def test_activity_poll_in_productive_batches_does_not_form_a_loop(root, home):
    state = {"requests": 0, "id": 0}
    source = root / "productive-batches.txt"
    source.write_text("one\ntwo\nthree\n", encoding="utf-8")

    def route(_, body):
        state["requests"] += 1
        results = tool_results(body["messages"])
        running = next((text for text in results if text.startswith("[running] activity ")), "")
        if not running:
            return tool_call("run", {"command": "sleep 30", "yield_ms": 250})
        match = re.search(r"activity (\d+)", running)
        assert_true(match is not None, running)
        state["id"] = int(match.group(1))
        batch = state["requests"] - 1
        if batch <= 3:
            return tool_calls(
                [
                    (
                        f"poll-{batch}",
                        "activity",
                        {"operation": "poll", "id": state["id"]},
                    ),
                    (
                        f"read-{batch}",
                        "read_path",
                        {"path": str(source), "offset": batch, "limit": 1},
                    ),
                ]
            )
        return event({"content": "productive-batches-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_AUTO_COMPACT_PCT"] = "0"
        env["UAGENT_AUTO_COMPACT_TOKENS"] = "0"
        result = run(root, env, "--yolo", "-p", "monitor and inspect", timeout=8)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("productive-batches-ok"), result.stdout)
        assert_true(len(server.requests) == 5, len(server.requests))


def test_tool_call_budget_is_unlimited_by_default(root, home):
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
        result = run(root, env, "--yolo", "-p", "read every line")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("unlimited-tools-ok"), result.stdout)


def test_image_fallback_reaches_another_provider(root, home):
    """A vision route on a different provider stands in for native image input."""
    picture = root / "shot.png"
    picture.write_bytes(SMALL_PNG)

    def describe(_, body):
        assert_true(body.get("model") == "vision-model", body.get("model"))
        assert_true("tools" not in body, body)
        serialized = json.dumps(body["messages"])
        assert_true("image_url" in serialized, serialized[:200])
        return event({"content": "a terminal showing a stack trace"})

    vision = Server([describe])

    def reject_image(handler, _):
        write_json_response(
            handler,
            {
                "error": {
                    "type": "invalid_request_error",
                    "code": "unsupported_value",
                    "message": "this model does not support image input",
                }
            },
            status=400,
        )

    def after_fallback(_, body):
        serialized = json.dumps(body["messages"])
        assert_true("image_url" not in serialized, serialized[:200])
        assert_true("described, not seen" in serialized, serialized[:400])
        assert_true("stack trace" in serialized, serialized[:400])
        # The conversation model is never told which way images arrived. The
        # check spans every role: runtime context is a user turn now.
        system = "\n".join(str(message.get("content", "")) for message in body["messages"])
        assert_true("vision model" not in system, system)
        assert_true("Image input unavailable" not in system, system)
        return event({"content": "saw-it"})

    parent = Server([reject_image, after_fallback])
    try:
        env = base_env(home, parent.url)
        env["UAGENT_PROVIDERS"] = json.dumps(
            {"eyes": {"base_url": vision.url, "api_key": "eyes-key"}}
        )
        env["UAGENT_IMAGE_MODEL"] = "eyes/vision-model"
        result = run(
            root,
            env,
            "--yolo",
            "--attach",
            str(picture),
            "-p",
            "what is this",
            timeout=20,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "saw-it", result.stdout)
        assert_true(len(vision.requests) == 1, len(vision.requests))
    finally:
        parent.close()
        vision.close()


def test_headless_reaps_timed_out_process(root, home):
    workspace = root / "timed-out-process-workspace"
    workspace.mkdir()
    pid_file = workspace / "pid"
    command = (
        f"echo $$ > {shlex.quote(str(pid_file))}; printf 'partial-before-timeout\\n'; sleep 30"
    )
    with Server([tool_call("run", {"command": command})]) as server:
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


TESTS = (
    test_prompt_overlay_replaces_base_sections,
    test_plain_turn,
    test_adaptive_system_revises_replaces_and_clears,
    test_stream_error_is_not_an_empty_response,
    test_empty_response_after_tools_recovers,
    test_foreign_tool_markup_recovers_as_prose,
    test_transient_stream_errors_retry_before_progress,
    test_config_reload_applies_only_at_turn_boundaries,
    test_project_instructions_precede_first_turn,
    test_session_title_replaces_initial_greeting,
    test_input_steering_yields_activity_wait,
    test_input_idle_background_completion_is_observational,
    test_headless_json_envelope_contains_trace_usage_and_exit,
    test_headless_json_stream_emits_lifecycle_events,
    test_session_budget_stops_before_the_next_call,
    test_turn_cost_is_unlimited_by_default,
    test_tool_policy_scopes_schema_and_runtime,
    test_project_agent_config_trust,
    test_memory_reaches_context_by_scope,
    test_configured_redaction_keywords_apply,
    test_memory_background_extractor_is_bounded,
    test_memory_background_extractor_releases_failed_claims,
    test_no_memory_hides_index_and_tool,
    test_first_event_timeout,
    test_midturn_compaction_preserves_progress_and_usage,
    test_absolute_compaction_ceiling,
    test_activity_progress_polls_do_not_trip_identical_call_guard,
    test_activity_no_change_polls_are_steered_then_stopped,
    test_activity_poll_in_productive_batches_does_not_form_a_loop,
    test_tool_call_budget_is_unlimited_by_default,
    test_image_fallback_reaches_another_provider,
    test_headless_reaps_timed_out_process,
)
