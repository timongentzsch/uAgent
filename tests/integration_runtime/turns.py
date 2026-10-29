import json

from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    function_names,
    has_message,
    run,
    tool_call,
    tool_results,
)


def test_plain_turn(root, home, *, binary):
    with Server([event({"content": "ok"})]) as server:
        result = run(root, base_env(home, server.url), "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "ok", result.stdout)


def test_the_offered_tools_hold_while_background_work_starts(root, home, *, binary):
    """The tool array is part of the cached prompt: starting background work
    once added `activity` mid-turn and rewrote the cache (9.6k tokens live)."""
    offered = []

    def route(_, body):
        if has_message(body["messages"], "user", "background-brief"):
            return event({"content": "child-done"})
        offered.append(sorted(function_names(body)))
        if len(offered) == 1:
            return tool_call("subagent", {"prompt": "background-brief", "background": True})
        return event({"content": "parent-done"})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "go", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
    assert_true(len(offered) == 2 and offered[0] == offered[1], offered)
    assert_true("activity" in offered[0], offered)


def test_instructions_follow_the_base(root, home, *, binary):
    (home / ".uagent").mkdir(parents=True, exist_ok=True)
    (home / ".uagent" / "AGENTS.md").write_text("user-rule")
    (root / "AGENTS.md").write_text("project-rule")
    with Server([event({"content": "unexpected model call"})]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "--show-system-prompt", "--json", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        effective = json.loads(result.stdout)["effective"]
        assert_true(
            effective.find("Read only what the task needs") < effective.find("user-rule"), effective
        )
        assert_true(effective.find("user-rule") < effective.find("project-rule"), effective)
        assert_true(server.requests == [], server.requests)


def test_stream_error_is_not_an_empty_response(root, home, *, binary):
    with Server(
        [{"error": {"message": "upstream overloaded", "type": "server_error"}}],
        repeat_last=True,
    ) as server:
        result = run(root, base_env(home, server.url), "-p", "reply", binary=binary)
        assert_true(result.returncode != 0, result.stdout)
        assert_true("upstream overloaded" in result.stderr, result.stderr)
        assert_true("empty response" not in result.stderr, result.stderr)
        assert_true(len(server.requests) == 3, server.requests)


def test_partial_stop_policy_continues_prose_and_salvages_calls(root, home, *, binary):
    def completed_after_partial(_, body):
        assistants = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "assistant"
        ]
        notes = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "user"
        ]
        valid = "partial answer" in assistants and any(
            "partial model response: length" in note for note in notes
        )
        return event({"content": "continued-ok" if valid else "continued-bad"})

    with Server(
        [event({"content": "partial answer"}, finish="length"), completed_after_partial]
    ) as server:
        result = run(root, base_env(home, server.url), "-p", "finish safely", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "continued-ok", result.stdout)
        assert_true(len(server.requests) == 2, len(server.requests))

    cutoff_call = tool_call("read_path", {"path": "."}, call_id="cutoff-call")
    cutoff_call["choices"][0]["finish_reason"] = "length"

    def completed_after_call(_, body):
        results = tool_results(body["messages"])
        valid = len(results) == 1 and "cutoff-call" in json.dumps(body["messages"])
        return event({"content": "call-salvaged" if valid else "call-lost"})

    with Server([cutoff_call, completed_after_call]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "inspect", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "call-salvaged", result.stdout)
        assert_true(len(server.requests) == 2, len(server.requests))

    unknown_call = tool_call("read_path", {"path": "."}, call_id="unknown-call")
    unknown_call["choices"][0]["finish_reason"] = "provider_new_reason"

    def completed_after_unknown(_, body):
        notes = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "user"
        ]
        valid = not tool_results(body["messages"]) and any(
            "partial model response: provider_new_reason" in note for note in notes
        )
        return event({"content": "unknown-recovered" if valid else "unknown-unsafe"})

    with Server([unknown_call, completed_after_unknown]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "inspect", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "unknown-recovered", result.stdout)
        assert_true(len(server.requests) == 2, len(server.requests))

    with Server([event({"content": "unsafe partial"}, finish="content_filter")]) as server:
        result = run(root, base_env(home, server.url), "-p", "answer", binary=binary)
        assert_true(result.returncode != 0, result.stdout)
        assert_true("content_filter" in result.stderr, result.stderr)
        assert_true(len(server.requests) == 1, len(server.requests))


def test_empty_partial_stop_continues_once_then_fails(root, home, *, binary):
    empty_thinking = event(
        {},
        finish="length",
        usage={
            "prompt_tokens": 1,
            "completion_tokens": 4,
            "completion_tokens_details": {"reasoning_tokens": 4},
        },
    )

    def again(_, body):
        # Nothing arrived to keep, so no empty assistant message is replayed:
        # the model is only told to carry on.
        assert_true("partial model response: length" in json.dumps(body["messages"]), body)
        assert_true(body["messages"][-2]["role"] != "assistant", body["messages"])
        return empty_thinking

    with Server([empty_thinking, again]) as server:
        result = run(root, base_env(home, server.url), "-p", "finish safely", binary=binary)
        assert_true(result.returncode == 1, result.stdout)
        assert_true(
            "model response stopped before completion (length)" in result.stderr,
            result.stderr,
        )
        assert_true(len(server.requests) == 2, server.requests)


def test_empty_response_after_tools_recovers(root, home, *, binary):
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
        result = run(root, base_env(home, server.url), "--yolo", "-p", "inspect", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "recovered", result.stdout)
        assert_true(len(server.requests) == 4, len(server.requests))


def test_foreign_tool_markup_recovers_as_prose(root, home, *, binary):
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
        result = run(root, base_env(home, server.url), "--yolo", "-p", "answer", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "markup-recovered", result.stdout)

    with Server([event({"content": markup}), event({"content": markup})]) as repeated:
        result = run(root, base_env(home, repeated.url), "--yolo", "-p", "answer", binary=binary)
        assert_true(result.returncode != 0, result.stdout)
        assert_true("repeatedly returned invalid tool markup" in result.stderr, result.stderr)
        assert_true(len(repeated.requests) == 2, len(repeated.requests))

    # A bracketed marker followed by JSON is call syntax this harness does not
    # execute -- there is no parser for it at all. It is recognized only so the
    # turn can ask for a real call instead of printing the markup as an answer,
    # and the arguments here would be valid and auto-approved if anything ran
    # them.
    marker = home / "bracket-markup-must-not-run"
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
        result = run(root, base_env(home, native.url), "--yolo", "-p", "answer", binary=binary)
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
        result = run(root, base_env(home, exhausted.url), "--yolo", "-p", "inspect", binary=binary)
        assert_true(result.returncode != 0, result.stdout)
        assert_true("model returned an empty response" in result.stderr, result.stderr)
        assert_true(len(exhausted.requests) == 4, len(exhausted.requests))


def test_transient_stream_errors_retry_before_progress(root, home, *, binary):
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
        trace = root / "retry.jsonl"
        result = run(
            root, base_env(home, server.url), f"--debug={trace}", "-p", "reply", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "retry-ok", result.stdout)
        assert_true(len(server.requests) == 3, server.requests)
        # The answering attempt's duration leaves the backoff before it out;
        # only the end-to-end time carries it.
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        timing = next(r["data"] for r in records if r["event"] == "model_response")
        assert_true(timing["duration_ms"] < 1000 <= timing["end_to_end_ms"], timing)
