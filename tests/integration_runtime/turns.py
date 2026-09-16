import json

from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    function_names,
    run,
    tool_call,
    tool_results,
)


def test_plain_turn(root, home, *, binary):
    def reply(_, body):
        names = function_names(body)
        assert_true("activity" not in names, names)
        return event({"content": "ok"}, usage={"prompt_tokens": 2, "completion_tokens": 1})

    with Server([reply]) as server:
        result = run(root, base_env(home, server.url), "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "ok", result.stdout)


def test_prompt_documents_refresh_and_standalone_inspection(root, home, *, binary):
    global_path = home / ".uagent" / "system-prompt.json"
    project_path = root / ".uagent" / "system-prompt.json"
    project_path.parent.mkdir(parents=True, exist_ok=True)
    global_path.parent.mkdir(parents=True, exist_ok=True)
    global_path.write_text(json.dumps({"mode": "overlay", "text": "global-old"}))
    project_path.write_text(json.dumps({"mode": "replace", "text": "project replacement"}))
    with Server([event({"content": "unexpected model call"})]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "--show-system-prompt", "--json", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        described = json.loads(result.stdout)
        assert_true(described["effective"].startswith("project replacement\n\n"), described)
        assert_true("global-old" not in described["effective"], described)
        assert_true(server.requests == [], server.requests)
    project_path.unlink()

    def initial(_, body):
        assert_true("global-old" in body["messages"][0]["content"], body)
        global_path.write_text(json.dumps({"mode": "overlay", "text": "global-new"}))
        return tool_call(
            "uagent", {"action": "inspect", "topic": "prompt"}, call_id="inspect-prompt"
        )

    def refreshed(_, body):
        prompt = body["messages"][0]["content"]
        assert_true("global-new" in prompt and "global-old" not in prompt, prompt)
        described = json.loads(tool_results(body["messages"])[-1])
        assert_true(described["effective"] == prompt, described)
        return event({"content": "prompt-refreshed"})

    with Server([initial, refreshed]) as server:
        result = run(root, base_env(home, server.url), "-p", "inspect", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "prompt-refreshed", result.stdout)
    global_path.write_text("invalid json")
    with Server([event({"content": "unexpected model call"})]) as server:
        result = run(root, base_env(home, server.url), "-p", "inspect", binary=binary)
        assert_true(result.returncode != 0, result.stdout)
        assert_true("Invalid system prompt" in result.stderr, result.stderr)
        assert_true(server.requests == [], server.requests)
        assert_true(global_path.read_text() == "invalid json", "invalid file was changed")


def test_adaptive_system_revises_replaces_and_clears(root, home, *, binary):
    def initial(_, body):
        assert_true("adapt_system" in function_names(body), function_names(body))
        assert_true("MUTABLE SELF-DIRECTIVE" not in body["messages"][0]["content"], body)
        return tool_call(
            "adapt_system",
            {
                "action": "set",
                "revision": "0",
                "text": "Inspect broadly and challenge the initial hypothesis.",
                "reason": "The task is still ambiguous.",
            },
            call_id="adapt-1",
        )

    def replace(_, body):
        system = body["messages"][0]["content"]
        assert_true("MUTABLE SELF-DIRECTIVE" not in system, system)
        assert_true("Inspect broadly and challenge" in system, system)
        assert_true(
            system.rfind("[HOST CAPABILITIES]") > system.rfind("Inspect broadly and challenge"),
            system,
        )
        return tool_call(
            "adapt_system",
            {
                "action": "set",
                "revision": "1",
                "text": "Stop broad exploration and validate the localized invariant.",
                "reason": "New evidence localized the issue.",
            },
            call_id="adapt-2",
        )

    def clear(_, body):
        system = body["messages"][0]["content"]
        assert_true("MUTABLE SELF-DIRECTIVE" not in system, system)
        assert_true("validate the localized invariant" in system, system)
        assert_true("Inspect broadly and challenge" not in system, system)
        return tool_call(
            "adapt_system",
            {"action": "reset", "revision": "2", "reason": "Specialized execution is complete."},
            call_id="adapt-3",
        )

    def finish(_, body):
        system = body["messages"][0]["content"]
        assert_true("MUTABLE SELF-DIRECTIVE" not in system, system)
        results = tool_results(body["messages"])
        assert_true(any("Next model request" in result for result in results), results)
        return event({"content": "adaptive-system-ok"})

    with Server([initial, replace, clear, finish]) as server:
        trace = root / "adaptive-system.jsonl"
        env = base_env(home, server.url)
        env["UAGENT_ADAPT_SYSTEM"] = "1"
        result = run(
            root, env, "--yolo", f"--debug={trace}", "-p", "adapt as needed", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("adaptive-system-ok"), result.stdout)
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        revisions = [
            int(record["data"]["revision"])
            for record in records
            if record.get("event") == "prompt_changed"
            and record["data"].get("scope") == "conversation"
        ]
        assert_true(revisions == [1, 2, 3], revisions)
        snapshots = [record["data"] for record in records if record.get("event") == "model_request"]
        assert_true([item["system_revision"] for item in snapshots] == [0, 1, 2, 3], snapshots)

    def static_reply(_, body):
        assert_true("adapt_system" not in function_names(body), function_names(body))
        return event({"content": "static-system-ok"})

    with Server([static_reply]) as server:
        result = run(root, base_env(home, server.url), "-p", "work normally", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "static-system-ok", result.stdout)


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


def test_empty_partial_stop_fails_without_invalid_continuation(root, home, *, binary):
    empty_thinking = event(
        {},
        finish="length",
        usage={
            "prompt_tokens": 1,
            "completion_tokens": 4,
            "completion_tokens_details": {"reasoning_tokens": 4},
        },
    )
    with Server([empty_thinking]) as server:
        result = run(root, base_env(home, server.url), "-p", "finish safely", binary=binary)
        assert_true(result.returncode == 1, result.stdout)
        assert_true(
            "model response stopped before completion (length)" in result.stderr,
            result.stderr,
        )
        assert_true(len(server.requests) == 1, server.requests)


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
        result = run(root, base_env(home, server.url), "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "retry-ok", result.stdout)
        assert_true(len(server.requests) == 3, server.requests)
