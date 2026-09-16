import json
import pathlib
import stat
import time

from integration_support import (
    Server,
    assert_true,
    base_env,
    budget,
    event,
    has_message,
    run,
    run_dialog,
    run_pty,
    session_files,
    tool_call,
    tool_results,
    write_sse_sequence,
)


def test_config_reload_applies_only_at_turn_boundaries(root, home, *, binary):
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
            root, base_env(home, server.url), "first\nsecond\n/q\n", timeout=12, binary=binary
        )
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true("first-turn-used-snapshot" in result.stdout, result.stdout)
        assert_true("configuration reloaded for the next turn" in result.stdout, result.stdout)
        assert_true("tool call limit reached (1)" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 3, server.requests)


def test_prompt_overlay_reaches_the_live_prompt(root, home, *, binary):
    """The overlay file reaches the request; its semantics are protocol_test.cc's."""
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
        valid = "OVERLAY-ANSWER-RULE" in prompt and prompt.index("OVERLAY-TAIL") < prompt.index(
            "[HOST CAPABILITIES]"
        )
        return event({"content": "overlay-ok" if valid else f"overlay-bad: {prompt[:400]}"})

    with Server([verify]) as server:
        env = base_env(home, server.url)
        env["UAGENT_PROMPT_OVERLAY"] = str(overlay)
        result = run(workspace, env, "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "overlay-ok", result.stdout)

    # An overlay file that cannot be parsed is ignored rather than fatal. Only
    # the loader sees this: ApplyPromptOverlay is handed parsed json.
    def baseline(_, body):
        prompt = body["messages"][0].get("content", "")
        return event({"content": "base-ok" if "Cite code as path:line" in prompt else "base-bad"})

    overlay.write_text("{not json", encoding="utf-8")
    with Server([baseline]) as server:
        env = base_env(home, server.url)
        env["UAGENT_PROMPT_OVERLAY"] = str(overlay)
        result = run(workspace, env, "-p", "reply", binary=binary)
        assert_true(result.stdout.strip() == "base-ok", result.stdout)


def test_project_instructions_precede_first_turn(root, home, *, binary):
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
        result = run(nested, base_env(home, server.url), "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "instructions-ok", result.stdout)


def test_session_journal_records_digests_not_argument_values(root, home, *, binary):
    """The journal must say what happened without saying what was in it.

    Argument values stay out by design, which also meant a later reader could
    not tell an identical repeated call from a new one, or say which failure a
    tool hit. A digest and the error code answer both and leak neither.
    """
    secret = root / "canary-argument-name.txt"
    secret.write_text("body\n", encoding="utf-8")

    def read_once(_, __):
        return tool_call("read_path", {"path": secret.name})

    def read_again(_, body):
        assert_true(all("_uagent_read_range" not in m for m in body["messages"]), body)
        return tool_call("read_path", {"path": secret.name}, call_id="call-2")

    def read_missing(_, __):
        return tool_call("read_path", {"path": "absent.txt"}, call_id="call-3")

    responders = [read_once, read_again, read_missing, event({"content": "journal-ok"})]
    with Server(responders) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"inspect\n", b"journal-ok"), b"", b"/q\n"],
            binary=binary,
        )
        assert_true(code == 0, output)

    sessions = session_files(home)
    state = json.loads(sessions[0].read_text(encoding="utf-8").splitlines()[1])
    saved_results = [m for m in state["messages"] if m.get("role") == "tool"]
    assert_true(saved_results[0].get("_uagent_read_range") == [secret.name, 1, 1], saved_results)
    assert_true("_uagent_read_range" not in saved_results[-1], saved_results)
    journal = pathlib.Path(str(sessions[0]) + ".events.jsonl")
    text = journal.read_text(encoding="utf-8")
    records = [json.loads(line) for line in text.splitlines()]
    calls = [r["data"] for r in records if r["type"] == "tool.call"]
    results = [r["data"] for r in records if r["type"] == "tool.result"]
    assert_true(len(calls) == 3, calls)

    digests = [call.get("arguments_digest", "") for call in calls]
    assert_true(all(len(digest) == 12 for digest in digests), digests)
    # The repeat matches; the different path does not.
    assert_true(digests[0] == digests[1], digests)
    assert_true(digests[2] != digests[0], digests)

    failed = [r for r in results if r.get("status") != "ok"]
    assert_true(len(failed) == 1, results)
    assert_true(failed[0].get("error_code") == "not_found", failed)

    # The value itself never reaches the journal, digest or not.
    assert_true("canary-argument-name" not in text, "argument value leaked")
    assert_true("absent.txt" not in text, "argument value leaked")


def test_session_title_replaces_initial_greeting(root, home, *, binary):
    with Server([event({"content": "hello-ok"}), event({"content": "task-ok"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"hello\n", b"hello-ok", b"Ready", None),
                (b"investigate browser efficiency\n", b"task-ok", b"Ready", None),
                b"/q\n",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"task-ok" in output, output)
        assert_true(len(server.requests) == 2, server.requests)
        sessions = session_files(home)
        assert_true(len(sessions) == 1, sessions)
        header = json.loads(sessions[0].read_text(encoding="utf-8").splitlines()[0])
        assert_true(header["title"] == "investigate browser efficiency", header)
        journal = pathlib.Path(str(sessions[0]) + ".events.jsonl")
        assert_true(journal.exists(), journal)
        assert_true(stat.S_IMODE(journal.stat().st_mode) == 0o600, journal.stat())
        records = [json.loads(line) for line in journal.read_text().splitlines()]
        types = [record["type"] for record in records]
        assert_true(types[0] == "session.ready", types)
        assert_true("session.ended" not in types, "detaching ended the shared runtime")
        assert_true(types.count("turn.started") == 2, types)
        assert_true(types.count("turn.completed") == 2, types)
        assert_true("investigate browser efficiency" not in journal.read_text(), records)


def test_input_steering_yields_activity_wait(root, home, *, binary):
    def route(_, body):
        messages = body["messages"]
        results = tool_results(messages)
        if has_message(messages, "user", "change course"):
            yielded = any("wait yielded for queued steering" in result for result in results)
            still_running = any("1 activity still running" in result for result in results)
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
            binary=binary,
        )
        elapsed = time.monotonic() - started
        assert_true(code == 0, output)
        assert_true(b"steering-wait-ok" in output, output)
        assert_true(b"steering-wait-bad" not in output, output)
        # Same wall-clock claim as the run_pty deadline above it, so the same
        # scaling: an instrumented build is slower without being wrong.
        assert_true(elapsed < budget(8), elapsed)


def test_steer_then_escape_resumes_without_interrupt_notice(root, home, *, binary):
    def route(handler, body):
        messages = body["messages"]
        if has_message(messages, "user", "change course"):
            return event({"content": "steering-resumed-ok"})
        write_sse_sequence(
            handler,
            [event({"content": f"slow chunk {index} "}, finish=None) for index in range(12)]
            + [event({"content": "slow tail"})],
            delay=0.25,
        )
        return None

    with Server([route]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"start\n", b"slow chunk 0"),
                (b"change course\n\x1b", b"steering-resumed-ok"),
                b"/q\n",
            ],
            args=("--yolo",),
            timeout=20,
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"steering-resumed-ok" in output, output)
        assert_true("· interrupted" not in output.decode("utf-8", "replace"), output)


def test_input_idle_background_completion_is_observational(root, home, *, binary):
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
                (b"", b"notified completed"),
                (b"next\n", b"background-ui-only"),
                b"/q\n",
            ],
            args=("--yolo",),
            timeout=8,
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"notified" in output, output)
        assert_true(b"background-ui-only" in output, output)
        assert_true(b"background-leaked" not in output, output)
        assert_true(len(server.requests) == 3, server.requests)


def test_headless_json_envelope_contains_trace_usage_and_exit(root, home, *, binary):
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
            root, base_env(home, server.url), "--yolo", "-p", "run a tool", "--json", binary=binary
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


def test_headless_json_stream_emits_lifecycle_events(root, home, *, binary):
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
            binary=binary,
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
