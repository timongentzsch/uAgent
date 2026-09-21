import json
import os
import pathlib
import re
import shlex
import time

from integration_support import (
    SMALL_PNG,
    Server,
    assert_true,
    base_env,
    budget,
    event,
    midturn_compaction_env,
    run,
    sse,
    tool_call,
    tool_calls,
    tool_results,
    write_json_response,
    write_session,
)


def test_first_event_timeout(root, home, *, binary):
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
        result = run(root, env, "-p", "probe", binary=binary)
        elapsed = time.monotonic() - started
        assert_true(result.returncode == 1, result.returncode)
        assert_true("no event within 1s" in result.stderr, result.stderr)
        assert_true(len(server.requests) == 3, server.requests)
        # The lower bound is the behaviour -- three attempts each waiting out a
        # 1s first-event timeout -- so it is absolute. The upper bound only
        # says the retries ended, which is a wall-clock claim about the host
        # and scales with an instrumented build like every other deadline.
        assert_true(3.0 < elapsed < budget(6.5), elapsed)


def test_provider_retry_after_is_a_minimum_delay(root, home, *, binary):
    def overloaded(handler, _):
        data = json.dumps(
            {
                "error": {
                    "message": "temporarily overloaded",
                    "type": "service_unavailable_error",
                    "code": "server_is_overloaded",
                }
            }
        ).encode()
        handler.send_response(503)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(data)))
        handler.send_header("Retry-After", "1")
        handler.end_headers()
        handler.wfile.write(data)

    with Server([overloaded, event({"content": "retry-after-ok"})]) as server:
        started = time.monotonic()
        result = run(root, base_env(home, server.url), "-p", "reply", binary=binary)
        elapsed = time.monotonic() - started
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "retry-after-ok", result.stdout)
        assert_true(len(server.requests) == 2, server.requests)
        assert_true(elapsed >= 1.0, elapsed)


def test_midturn_compaction_preserves_progress_and_usage(root, home, *, binary):
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
        result = run(root, env, "--yolo", f"--debug={trace}", "-p", "inspect", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("midturn-finished-ok" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 4, server.requests)
        records = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        turn_end = next(record for record in records if record["event"] == "turn_end")
        assert_true(turn_end["data"]["usage"]["input"] == 2090, turn_end)
        assert_true(turn_end["data"]["usage"]["output"] == 30, turn_end)
        folds = [record for record in records if record["event"] == "midturn_compact"]
        assert_true(len(folds) == 1, folds)


def test_absolute_compaction_ceiling(root, home, *, binary):
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
        result = run(root, env, "-c", f"--debug={trace}", "-p", "continue", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("absolute-compact-ok"), result.stdout)
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        compacted = [record for record in records if record["event"] == "auto_compact"]
        assert_true(len(compacted) == 1, compacted)
        assert_true(compacted[0]["data"]["projected_tokens"] >= 2000, compacted)


def test_activity_progress_polls_do_not_trip_identical_call_guard(root, home, *, binary):
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
        result = run(root, env, "--yolo", "-p", "monitor", timeout=12, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("progress-polls-ok"), result.stdout)
        assert_true(len(server.requests) == 6, len(server.requests))


def test_valid_repeated_calls_are_nudged_before_the_high_loop_ceiling(root, home, *, binary):
    state = {"requests": 0}
    source = root / "repeated-read.txt"
    source.write_text("stable evidence\n", encoding="utf-8")

    def route(_, body):
        state["requests"] += 1
        if state["requests"] <= 3:
            return tool_call("read_path", {"path": str(source)})
        combined = "\n".join(str(message.get("content", "")) for message in body["messages"])
        assert_true("[repeated tool advisory]" in combined, combined)
        assert_true("run 3 consecutive times" in combined, combined)
        return event({"content": "repetition-recovered"})

    with Server([route]) as server:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            "-p",
            "inspect repeatedly",
            timeout=8,
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("repetition-recovered"), result.stdout)
        assert_true(len(server.requests) == 4, len(server.requests))


def test_activity_no_change_polls_are_steered_then_stopped(root, home, *, binary):
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
        if state["requests"] == 6:
            combined = "\n".join(str(message.get("content", "")) for message in body["messages"])
            assert_true("this turn ends in an error" in combined, combined)
        if state["requests"] <= 13:
            return tool_call("activity", {"operation": "poll", "id": state["id"]})
        return event({"content": "fourteenth-round-should-not-run"})

    with Server([route]) as server:
        result = run(
            root, base_env(home, server.url), "--yolo", "-p", "monitor", timeout=20, binary=binary
        )
        assert_true(result.returncode != 0, result.stdout)
        assert_true(
            f"activity {state['id']} is still running, but the model polled it 12 times"
            in result.stderr,
            result.stderr,
        )
        assert_true(len(server.requests) == 13, len(server.requests))


def test_activity_poll_in_productive_batches_does_not_form_a_loop(root, home, *, binary):
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
        result = run(root, env, "--yolo", "-p", "monitor and inspect", timeout=8, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("productive-batches-ok"), result.stdout)
        assert_true(len(server.requests) == 5, len(server.requests))


def test_image_fallback_reaches_another_provider(root, home, *, binary):
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
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "saw-it", result.stdout)
        assert_true(len(vision.requests) == 1, len(vision.requests))
    finally:
        parent.close()
        vision.close()


def test_headless_reaps_timed_out_process(root, home, *, binary):
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
            workspace, env, "--yolo", f"--debug={trace}", "-p", "probe", timeout=8, binary=binary
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
