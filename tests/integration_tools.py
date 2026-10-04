import json
import os
import pathlib
import shlex
import signal
import subprocess
import time

from integration_support import (
    SMALL_PNG,
    Server,
    assert_true,
    base_env,
    captured_log_path,
    detached_pid,
    event,
    function_names,
    has_message,
    json_sentinel_command,
    large_json_command,
    run,
    run_dialog,
    run_pty,
    session_files,
    signal_process_group,
    tool_call,
    tool_results,
)


def test_read_path_puts_media_in_context(root, home, *, binary):
    """The model can pull an image and a document into its own context, and the
    encoded bytes do not stay in history afterwards."""
    workspace = root / "attach-workspace"
    workspace.mkdir()
    png = workspace / "shot.png"
    png.write_bytes(SMALL_PNG)
    pdf = workspace / "paper.pdf"
    # Large enough to exceed the synthetic 4K context estimate if encoded
    # bytes are incorrectly offered to mid-turn compaction.
    pdf.write_bytes(b"%PDF-1.4\n" + b"\x00" * (1024 * 1024))
    seen = {}

    def route(_, body):
        messages = body["messages"]
        parts = [p for m in messages if isinstance(m.get("content"), list) for p in m["content"]]
        if parts:
            seen["image"] = any(p.get("type") == "image_url" for p in parts)
            seen["file"] = any(p.get("type") == "file" for p in parts)
            return event({"content": "attach-ok"})
        return event(  # both in one batch: read_path is parallel_safe
            {
                "tool_calls": [
                    {
                        "index": i,
                        "id": f"call-{i}",
                        "function": {
                            "name": "read_path",
                            "arguments": json.dumps({"path": str(path)}),
                        },
                    }
                    for i, path in enumerate((png, pdf))
                ]
            },
            finish="tool_calls",
        )

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_CONTEXT"] = "4096"
        trace = workspace / "trace.jsonl"
        result = run(
            workspace, env, "--yolo", f"--debug={trace}", "-p", "look", timeout=40, binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "attach-ok", result.stdout)
        assert_true(seen.get("image"), seen)
        assert_true(seen.get("file"), seen)
        assert_true(len(server.requests) == 2, len(server.requests))
        events = [json.loads(line) for line in trace.read_text().splitlines()]
        assert_true(not any(event["event"] == "midturn_compact" for event in events), events)
        # the base64 payload must not survive into the saved session
        sessions = session_files(home)
        blobs = [
            s for s in sessions if "base64," in s.read_text(encoding="utf-8", errors="replace")
        ]
        assert_true(not blobs, blobs)


def test_a_malformed_tool_call_is_answered_not_fatal(root, home, *, binary):
    """The model hears what was wrong with its call and sends it again."""
    workspace = root / "malformed-workspace"
    workspace.mkdir()
    (workspace / "note.txt").write_text("malformed-recovered\n", encoding="utf-8")

    def broken(arguments, name="read_path"):
        return event(
            {
                "tool_calls": [
                    {"index": 0, "id": "bad", "function": {"name": name, "arguments": arguments}}
                ]
            },
            finish="tool_calls",
        )

    def corrected(_, body):
        result = tool_results(body["messages"])[-1]
        assert_true("not one complete JSON object" in result, result)
        assert_true('{"path": "note' in result, result)
        return tool_call("read_path", {"path": "note.txt"})

    def nameless_then(_, body):
        # A call with no function name ran nothing; the note says so.
        assert_true("without a function name" in json.dumps(body["messages"]), body)
        return event({"content": "recovered-ok"})

    with Server(
        [
            broken('{"path": "note'),
            corrected,
            broken('{"path": "note.txt"}', name=""),
            nameless_then,
        ]
    ) as server:
        result = run(
            workspace,
            base_env(home, server.url),
            "--yolo",
            "-p",
            "read it",
            timeout=30,
            binary=binary,
        )
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(result.stdout.strip() == "recovered-ok", result.stdout)
        assert_true(len(server.requests) == 4, len(server.requests))


def test_a_loaded_skill_survives_compaction(root, home, *, binary):
    """The summary replaces the turns, not the procedure being followed."""
    workspace = root / "skill-compact-workspace"
    skill = workspace / ".uagent" / "skills" / "demo"
    skill.mkdir(parents=True)
    (skill / "SKILL.md").write_text(
        "---\nname: demo\ndescription: demo-description\n---\n\nkept-body-sentinel\n",
        encoding="utf-8",
    )

    def summarize(_, body):
        assert_true("tools" not in body, body)
        return event({"content": "the user is following the demo skill"})

    def after(_, body):
        serialized = json.dumps(body["messages"])
        assert_true("model-generated context summary" in serialized, serialized)
        assert_true("long-answer" not in serialized, serialized)
        kept = serialized.count("kept-body-sentinel")
        return event({"content": "skill-kept" if kept == 1 else f"skill-lost-{kept}"})

    with Server(
        [
            tool_call("skill", {"query": "demo"}),
            tool_call("skill", {"query": "demo"}),
            event({"content": "long-answer " * 400}),
            summarize,
            after,
        ]
    ) as server:
        result = run_dialog(
            workspace,
            base_env(home, server.url),
            "open the demo skill\n/compact\ncontinue\n/q\n",
            timeout=30,
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("skill-kept" in result.stdout, result.stdout)
        assert_true(len(server.requests) == 5, len(server.requests))


def test_an_attachment_over_the_budget_is_warned_about_once(root, home, *, binary):
    workspace = root / "budget-workspace"
    workspace.mkdir()
    for name in ("one.pdf", "two.pdf"):
        (workspace / name).write_bytes(b"%PDF-1.4\n" + b"\x00" * (700 * 1024))
    steps = [
        tool_call("read_path", {"path": "one.pdf"}, call_id="one"),
        tool_call("read_path", {"path": "two.pdf"}, call_id="two"),
        tool_call("run", {"command": "true"}, call_id="third"),
        tool_call("run", {"command": "true"}, call_id="fourth"),
        event({"content": "budget-ok"}),
    ]
    with Server(steps) as server:
        env = base_env(home, server.url)
        env["UAGENT_ATTACHMENT_MB"] = "1"
        result = run_dialog(workspace, env, "look\n/q\n", "--yolo", timeout=40, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("budget-ok" in result.stdout, result.stdout)
        said = result.stdout.count("request attachment budget exceeded")
        assert_true(said == 1, (said, result.stdout[-1500:]))


def test_full_run_and_python_terminal_trace(root, home, *, binary):
    shell_command = "printf 'shell-one\\n'\nprintf 'shell-two\\n'"
    python_code = "print('python-one')\nprint('python-two')"
    with Server(
        [
            tool_call("run", {"command": shell_command, "shell": "/bin/sh"}),
            tool_call(
                "write_file",
                {"path": ".uagent/scratch/trace.py", "content": python_code},
            ),
            tool_call("scratch", {"path": "trace.py"}),
            event({"content": "trace-ok"}),
        ]
    ) as server:
        env = base_env(home, server.url)
        result = run_dialog(
            root, env, "trace\n/q\n", "--yolo", "--verbosity", "full", timeout=20, binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        for expected in (
            "printf 'shell-one",
            "printf 'shell-two",
            "shell-one",
            "shell-two",
            "Running trace.py",
            "python-one",
            "python-two",
            "trace-ok",
        ):
            assert_true(expected in result.stdout, result.stdout)


def test_large_run_output_is_recoverable(root, home, *, binary):
    command, expected_bytes = large_json_command()
    artifact = {}

    def inspect_result(_, body):
        result = tool_results(body["messages"])[-1]
        path = captured_log_path(result)
        artifact["path"] = path
        assert_true(len(result) <= 512, len(result))
        assert_true("FULL-END" in result, result)
        assert_true("HEAD-ONLY" not in result, result)
        assert_true(path.exists(), path)
        assert_true(path.stat().st_size == expected_bytes, path.stat().st_size)
        return tool_call("run", {"command": json_sentinel_command(path)})

    def verify_recovery(_, body):
        results = tool_results(body["messages"])
        recovered = results[-1].strip() == "HEAD-ONLY"
        return event({"content": "artifact-ok" if recovered else "artifact-bad"})

    with Server(
        [
            tool_call("run", {"command": command}),
            inspect_result,
            verify_recovery,
        ]
    ) as server:
        env = base_env(home, server.url)
        env["UAGENT_TOOL_RESULT_CHARS"] = "512"
        env["UAGENT_CONTEXT"] = "131072"
        result = run(root, env, "--yolo", "-p", "inspect large output", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "artifact-ok", result.stdout)
        assert_true(artifact["path"].exists(), artifact)


def test_run_rejects_bare_python_before_execution(root, home, *, binary):
    def after_python(_, body):
        results = tool_results(body["messages"])
        assert_true(any("use scratch" in value for value in results), results)
        return event({"content": "guarded"})

    with Server([tool_call("run", {"command": "python -c 'print(1)'"}), after_python]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "work", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "guarded", result.stdout)


def test_delete_file_removes_and_receipts(root, home, *, binary):
    """Deleting reports what was removed, and refuses what it must not touch.

    The receipt is what a person reads before approving, so it carries the
    removed lines rather than a count.
    """
    workspace = root / "delete-workspace"
    workspace.mkdir(parents=True)
    doomed = workspace / "stale.txt"
    doomed.write_text("alpha\nbeta\n", encoding="utf-8")
    keep = workspace / "keep.txt"
    keep.write_text("KEEP\n", encoding="utf-8")

    def remove(_, body):
        assert_true("delete_file" in function_names(body), function_names(body))
        return tool_call("delete_file", {"path": "stale.txt"})

    def remove_missing(_, body):
        result = tool_results(body["messages"])[-1]
        assert_true("deleted" in result, result)
        return tool_call("delete_file", {"path": "stale.txt"}, call_id="call-2")

    def finish(_, body):
        result = tool_results(body["messages"])[-1]
        assert_true("error:" in result and "does not exist" in result, result)
        return event({"content": "delete-ok"})

    with Server([remove, remove_missing, finish]) as server:
        result = run(
            workspace, base_env(home, server.url), "--yolo", "-p", "remove it", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("delete-ok"), result.stdout)
        assert_true(not doomed.exists(), "file was not deleted")
        assert_true(keep.read_text() == "KEEP\n", "an unrelated file changed")


def test_grep_tool_round_trip(root, home, *, binary):
    workspace = root / "grep-workspace"
    workspace.mkdir()
    (workspace / "one.cpp").write_text("alpha\nproject_wide_symbol\nomega\n", encoding="utf-8")
    (workspace / "ignored.txt").write_text("project_wide_symbol\n", encoding="utf-8")

    def overshoot(_, body):
        result = tool_results(body["messages"])[0]
        valid = (
            "one.cpp" in result
            and "ignored.txt" not in result
            and "project_wide_symbol" in result
            and "alpha" in result
            and "omega" in result
        )
        if not valid:
            return event({"content": "grep-bad"})
        return tool_call(
            "grep",
            {"pattern": "project_wide_symbol", "path": ".", "context": 40},
            call_id="call-2",
        )

    def final(_, body):
        # A pacing hint past its bound is clamped rather than rejected, and the
        # result leads with the reduction instead of implying it got 40.
        result = tool_results(body["messages"])[-1]
        clamped = result.startswith("[clamped context to 10 of 40 requested]")
        return event({"content": "grep-ok" if clamped else f"grep-bad {result[:120]}"})

    with Server(
        [
            tool_call(
                "grep",
                {
                    "pattern": "project_wide_symbol",
                    "path": ".",
                    "glob": "*.cpp",
                    "context": 1,
                },
            ),
            overshoot,
            final,
        ]
    ) as server:
        env = base_env(home, server.url)
        result = run(workspace, env, "-p", "search", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "grep-ok", result.stdout)
        names = function_names(server.requests[0][1])
        assert_true("grep" in names, names)
        assert_true("show_image" not in names, names)


def test_skill_tool_offers_and_opens(root, home, *, binary):
    workspace = root / "skill-workspace"
    skill = workspace / ".uagent" / "skills" / "demo"
    skill.mkdir(parents=True)
    (skill / "SKILL.md").write_text(
        "---\nname: demo\ndescription: demo-description-sentinel\n---\n\ndemo-body-sentinel\n",
        encoding="utf-8",
    )
    unsupported = workspace / ".uagent" / "skills" / "unsupported"
    unsupported.mkdir()
    (unsupported / "SKILL.md").write_text(
        "---\ndescription: unsupported-sentinel\n"
        "requires-tools: missing-tool\n---\n\nunsupported-body\n",
        encoding="utf-8",
    )

    def offer(_, body):
        functions = {t["function"]["name"]: t["function"] for t in body.get("tools", [])}
        properties = functions.get("skill", {}).get("parameters", {}).get("properties", {})
        valid = (
            "skill" in functions
            and set(properties) == {"name", "query", "arguments"}
            and "enum" not in properties["name"]
            and properties["arguments"].get("maxLength") == 4096
            # Progressive disclosure: metadata rides every request; the body does not.
            and "demo-description-sentinel" in functions["skill"]["description"]
            and "unsupported-sentinel" not in functions["skill"]["description"]
            and "demo-body-sentinel" not in json.dumps(body)
        )
        if not valid:
            return event({"content": "schema-bad"})
        return tool_call("skill", {"query": "demo"})

    def confirm(_, body):
        opened = any("demo-body-sentinel" in str(m.get("content", "")) for m in body["messages"])
        return event({"content": "skill-ok" if opened else "skill-bad"})

    with Server([offer, confirm]) as server:
        code, output = run_pty(
            workspace,
            base_env(home, server.url),
            # Full output shows the opened skill body in the tool result.
            [
                (b"reply\n", b"skill-ok", b"Ready", None),
                b"\x04",
            ],
            columns=24,
            args=("--verbosity", "full"),
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"skill-ok" in output, output)
        assert_true(b"demo" in output, output)
        assert_true(b"demo-body-sentinel" in output, output)


def test_tool_trace_repeated_rounds_are_telemetry_only(root, home, *, binary):
    trace = root / "repeated-tools.jsonl"
    source = root / "rounds.txt"
    source.write_text("\n".join(str(i) for i in range(8)), encoding="utf-8")
    with Server(
        [
            tool_call("read_path", {"path": str(source), "offset": i, "limit": 1})
            for i in range(1, 9)
        ]
        + [event({"content": "rounds-finished"})]
    ) as server:
        env = base_env(home, server.url)
        env["UAGENT_AUTO_COMPACT_PCT"] = "0"
        env["UAGENT_INTERNAL_AUTO_COMPACT_TOKENS"] = "0"
        result = run(root, env, "--yolo", f"--debug={trace}", "-p", "inspect lines", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("rounds-finished"), result.stdout)
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        signals = [r for r in records if r["event"] == "repeated_tool_rounds"]
        assert_true(len(signals) == 1, signals)
        assert_true(signals[0]["data"]["tool"] == "read_path", signals)
        assert_true(signals[0]["data"]["rounds"] == 8, signals)


def test_invalid_tool_rejection_loop_stops_before_fourth_round(root, home, *, binary):
    trace = root / "rejected-tools.jsonl"
    dimensions = [(0, 80), (24, 0), (1001, 80)]
    responses = [
        tool_call(
            "activity",
            {
                "operation": "resize",
                "id": 1,
                "rows": rows,
                "cols": cols,
                "chars": "provider-default" * attempt,
                "wait_ms": attempt,
            },
        )
        for attempt, (rows, cols) in enumerate(dimensions, start=1)
    ]
    responses.append(event({"content": "fourth-round-should-not-run"}))
    with Server(responses) as server:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            f"--debug={trace}",
            "-p",
            "inspect",
            binary=binary,
        )
        assert_true(result.returncode != 0, result.stdout)
        assert_true(
            "equivalent rejected activity call 3 times "
            "(activity.invalid_dimensions)" in result.stderr,
            result.stderr,
        )
        assert_true(len(server.requests) == 3, len(server.requests))
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        loops = [r for r in records if r["event"] == "deterministic_rejection_loop"]
        assert_true(len(loops) == 1, loops)
        assert_true(loops[0]["data"]["issue_code"] == "activity.invalid_dimensions", loops)
        assert_true(loops[0]["data"]["issue_field"] == "", loops)
        assert_true(loops[0]["data"]["operation"] == "resize", loops)


def test_detached_terminal_materialized_wait_does_not_bypass_repeat_guard(root, home, *, binary):
    call = {"operation": "list", "wait_ms": 1}
    responses = [tool_call("activity", call) for _ in range(12)]
    responses.append(event({"content": "thirteenth-round-should-not-run"}))
    with Server(responses) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "list", binary=binary)
        assert_true(result.returncode != 0, result.stdout)
        assert_true(
            "model repeated the same tool call 12 times after two recovery instructions"
            in result.stderr,
            result.stderr,
        )
        assert_true(len(server.requests) == 12, len(server.requests))


def test_activity_wait_outlives_the_per_call_budget(root, home, *, binary):
    """A wait is bounded by wait_ms, not by the budget meant for running work."""
    workspace = root / "wait-budget-workspace"
    workspace.mkdir()
    marker = "slow-activity-finished"
    waited = 3

    def launch(*_):
        # Yielding to the background is what leaves a waitable activity; a
        # detached one is deliberately not waitable.
        return tool_call(
            "run",
            {"command": f"sleep {waited}; echo {marker}", "yield_ms": 250},
        )

    def wait_for_it(*_):
        return tool_call(
            "activity",
            {"operation": "wait", "mode": "all", "wait_ms": waited * 1000 + 4000},
            call_id="call-2",
        )

    def finish(_, body):
        result = tool_results(body["messages"])[-1]
        # The activity ran past the one-second per-call budget, so a truncated
        # wait would report the cap and leave the marker unseen.
        assert_true(marker in result, result)
        assert_true("capped by" not in result, result)
        return event({"content": "wait-budget-ok"})

    with Server([launch, wait_for_it, finish]) as server:
        env = base_env(home, server.url)
        env["UAGENT_TOOL_TIMEOUT"] = "1"
        started = time.time()
        result = run(workspace, env, "--yolo", "-p", "wait for it", timeout=60, binary=binary)
        elapsed = time.time() - started
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "wait-budget-ok", result.stdout)
        assert_true(elapsed > waited, f"returned in {elapsed:.1f}s, before the activity ended")


def test_parallel_run_overlaps(root, home, *, binary):
    """`run` is parallel_safe: independent commands must overlap, not queue."""
    sleep, count = 3, 4
    batch = event(
        {
            "tool_calls": [
                {
                    "index": i,
                    "id": f"call-{i}",
                    "function": {
                        "name": "run",
                        "arguments": json.dumps({"command": f"sleep {sleep}; echo done{i}"}),
                    },
                }
                for i in range(count)
            ]
        },
        finish="tool_calls",
    )
    with Server([batch, event({"content": "parallel-run-ok"})]) as server:
        started = time.time()
        result = run(
            root, base_env(home, server.url), "--yolo", "-p", "go", timeout=90, binary=binary
        )
        elapsed = time.time() - started
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "parallel-run-ok", result.stdout)
        # serial would be count*sleep; allow generous slack for spawn overhead
        assert_true(elapsed < sleep * count * 0.7, f"{elapsed:.1f}s for {count}x{sleep}s")


def test_detached_terminal_survives_and_is_readable(root, home, *, binary):
    workspace = root / "detached-terminal-workspace"
    workspace.mkdir()
    pid_file = workspace / "pid"
    command = (
        f"echo $$ > {shlex.quote(str(pid_file))}; "
        "i=0; while [ $i -lt 500 ]; do "
        "printf 'bounded-log-line-%04d-xxxxxxxxxxxxxxxx\\n' \"$i\"; i=$((i+1)); "
        "done; printf 'server-ready\\n'; sleep 30"
    )

    pid = None
    try:
        with Server(
            [
                tool_call(
                    "run",
                    {"command": command, "detach": True},
                ),
                event({"content": "launched"}),
            ]
        ) as launch_server:
            launched = run_dialog(
                workspace,
                base_env(home, launch_server.url),
                "launch the server\n/ps\n/q\n",
                "--yolo",
                timeout=8,
                binary=binary,
            )
            for _ in range(40):
                if pid_file.exists():
                    break
                time.sleep(0.05)
            assert_true(pid_file.exists(), "detached command did not start")
            pid = int(pid_file.read_text(encoding="utf-8"))
            assert_true(launched.returncode == 0, launched.stderr)
            assert_true("launched" in launched.stdout, launched.stdout)
            assert_true("background work" in launched.stdout, launched.stdout)
            assert_true("[detached] activity" in launched.stdout, launched.stdout)
            os.kill(pid, 0)

        with Server([event({"content": "fresh"})]) as fresh_server:
            fresh = run_dialog(
                workspace,
                base_env(home, fresh_server.url),
                "status\n/q\n",
                timeout=8,
                binary=binary,
            )
            assert_true(fresh.returncode == 0, fresh.stderr)
            assert_true("terminals:" not in fresh.stdout, fresh.stdout)

        def verify_reuse(_, body):
            result = tool_results(body["messages"])[-1]
            reused = (
                f"[detached] pid {pid}," in result
                and f"reused existing live activity id {pid}" in result
            )
            return event({"content": "terminal-reuse-ok" if reused else result})

        with Server(
            [
                tool_call("run", {"command": command, "detach": True}),
                verify_reuse,
            ]
        ) as reuse_server:
            reused = run(
                workspace,
                base_env(home, reuse_server.url),
                "--yolo",
                "-p",
                "launch the same detached server again",
                timeout=8,
                binary=binary,
            )
            assert_true(reused.returncode == 0, reused.stderr)
            assert_true(reused.stdout.strip() == "terminal-reuse-ok", reused.stdout)
            os.kill(pid, 0)

        def request_output(_, body):
            results = tool_results(body["messages"])
            listing = next(
                (text for text in results if text.startswith("[running] activity ")),
                "",
            )
            assert_true(f"activity {pid} " in listing, listing)
            return tool_call("activity", {"operation": "poll", "id": pid})

        def verify_output(_, body):
            results = tool_results(body["messages"])
            readable = any(
                text.startswith("[running · activity ") and "server-ready" in text
                for text in results
            )
            return event({"content": "terminal-ok" if readable else "terminal-bad"})

        def offer_output(_, body):
            names = function_names(body)
            assert_true("activity" in names, names)
            return tool_call("activity", {"operation": "list"})

        with Server([offer_output, request_output, verify_output]) as server:
            inspect_env = base_env(home, server.url)
            result = run(
                workspace,
                inspect_env,
                "--yolo",
                "-p",
                "inspect the server from this new session",
                timeout=8,
                binary=binary,
            )
            assert_true(result.returncode == 0, result.stderr)
            assert_true(result.stdout.strip() == "terminal-ok", result.stdout)
            os.kill(pid, 0)
            record = home / ".uagent" / "terminals" / f"{pid}.json"
            assert_true(record.exists(), record)
            record_data = json.loads(record.read_text(encoding="utf-8"))
            log = pathlib.Path(record_data["log"])
            assert_true("server-ready" in log.read_text(encoding="utf-8"), log)

        def verify_stop(_, body):
            result = tool_results(body["messages"])[-1]
            return event(
                {"content": "terminal-stop-ok" if "stopped process group" in result else result}
            )

        stop_call = tool_call("activity", {"operation": "stop", "id": pid})
        with Server([stop_call, verify_stop]) as stop_server:
            stop_env = base_env(home, stop_server.url)
            stopped = run(
                workspace,
                stop_env,
                "--yolo",
                "-p",
                "stop the detached server",
                timeout=8,
                binary=binary,
            )
            assert_true(stopped.returncode == 0, stopped.stderr)
            assert_true(stopped.stdout.strip() == "terminal-stop-ok", stopped.stdout)
            assert_true(not record.exists(), record)
            assert_true(not log.exists(), log)
            assert_true(not pathlib.Path(str(log) + ".1").exists(), log)
            try:
                os.killpg(pid, 0)
            except ProcessLookupError:
                pass
            else:
                raise AssertionError(f"detached process group {pid} survived activity stop")
            pid = None
    finally:
        signal_process_group(pid)


def test_log_pump_rotates_within_its_bound(root, _home, *, binary):
    """Detached terminals and MCP stderr share this pump; two half-size
    segments keep the log bounded without signalling the writer."""
    log = root / "pump.log"
    result = subprocess.run(
        [binary, "--log-pump", str(log), "4096"], input=b"x" * 20000, timeout=10
    )
    assert_true(result.returncode == 0, result.returncode)
    rotated = pathlib.Path(str(log) + ".1")
    assert_true(rotated.exists(), rotated)
    log_bytes = log.stat().st_size + rotated.stat().st_size
    assert_true(0 < log_bytes <= 4096, log_bytes)


def test_detached_terminal_tracks_group_after_wrapper_exit(root, home, *, binary):
    state = {"pid": None, "child": None}
    child_file = root / "detached-child-pid"
    child_tmp = root / "detached-child-pid.tmp"

    def kill_wrapper(_, body):
        state["pid"] = detached_pid(body)
        for _ in range(40):
            if child_file.exists():
                break
            time.sleep(0.05)
        assert_true(child_file.exists(), "detached child did not start")
        state["child"] = int(child_file.read_text(encoding="utf-8"))
        return tool_call("run", {"command": f"kill -KILL {state['pid']}"})

    def inspect_group(_, _body):
        return tool_call("activity", {"operation": "poll", "id": state["pid"]})

    def stop_group(_, body):
        result = tool_results(body["messages"])[-1]
        os.kill(state["child"], 0)
        tracked = result.startswith(f"[detached · activity {state['pid']} ")
        return (
            tool_call("activity", {"operation": "stop", "id": state["pid"]})
            if tracked
            else event({"content": "group-tracking-bad"})
        )

    def verify_group_stopped(_, body):
        result = tool_results(body["messages"])[-1]
        try:
            os.killpg(state["pid"], 0)
        except ProcessLookupError:
            gone = True
        else:
            gone = False
        cleaned = not (home / ".uagent" / "terminals" / f"{state['pid']}.json").exists()
        ok = "stopped process group" in result and gone and cleaned
        return event({"content": "group-tracking-ok" if ok else "group-tracking-bad"})

    server = Server(
        [
            tool_call(
                "run",
                {
                    "command": (
                        "sleep 20 & child=$!; "
                        f"printf '%s\\n' \"$child\" > {shlex.quote(str(child_tmp))} && "
                        f"mv {shlex.quote(str(child_tmp))} {shlex.quote(str(child_file))}; wait"
                    ),
                    "detach": True,
                },
            ),
            kill_wrapper,
            inspect_group,
            stop_group,
            verify_group_stopped,
        ]
    )
    try:
        env = base_env(home, server.url)
        result = run(root, env, "--yolo", "-p", "manage server", timeout=10, binary=binary)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(result.stdout.strip() == "group-tracking-ok", result.stdout)
    finally:
        server.close()
        signal_process_group(state["pid"], signal.SIGKILL)


def test_process_hardening_scrubs_loader_variables(root, home, *, binary):
    """Loader-injection variables never reach a spawned child process."""

    def inspect(_, __):
        return tool_call(
            "run",
            {"command": "echo preload=[${LD_PRELOAD:-unset}] limit=$(ulimit -c)"},
        )

    def finish(_, body):
        output = tool_results(body["messages"])[-1]
        assert_true("preload=[unset]" in output, output)
        assert_true("limit=0" in output, output)
        return event({"content": "hardening-ok"})

    with Server([inspect, finish]) as server:
        env = base_env(home, server.url)
        env["LD_PRELOAD"] = "/nonexistent-injection.so"
        result = run(root, env, "--yolo", "-p", "inspect", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("hardening-ok"), result.stdout)


def test_composite_configuration_requires_exact_human_approval(root, home, *, binary):
    """Safe credential references reach a redacted prompt, even under --yolo."""
    config = home / ".uagent" / ".config"
    config.parent.mkdir(parents=True, exist_ok=True)
    original = (
        "# keep me\n"
        "LOCAL_PROXY_API_KEY=adjacent-integration-secret\n"
        'UAGENT_PROVIDERS=\'{"old":{"base_url":"https://old.example/v1",'
        '"api_key":"$LOCAL_PROXY_API_KEY"}}\'\n'
    )
    config.write_text(original)
    proposed = json.dumps(
        {
            "codex-local": {
                "base_url": "http://127.0.0.1:8787/openai/v1",
                "api_key": "$CODEX_LOCAL_PROXY_API_KEY",
                "wire_api": "responses",
                "hosted_tools": ["web_search"],
            }
        },
        separators=(",", ":"),
    )

    def request_change(_, __):
        return tool_call(
            "uagent",
            {
                "action": "configure",
                "scope": "user",
                "changes": [{"key": "UAGENT_PROVIDERS", "operation": "set", "value": proposed}],
            },
        )

    def finish(_, body):
        result = tool_results(body["messages"])[-1]
        assert_true("wrote" in result and "needs a restart" in result, result)
        return event({"content": "composite-config-ok"})

    with Server([request_change, finish]) as server:
        status, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"configure providers\n", b"Allow uagent?"),
                (b"y\n", b"composite-config-ok"),
                b"/quit\n",
            ],
            args=("--yolo",),
            timeout=25,
            binary=binary,
        )
        assert_true(status == 0, output)
        assert_true(b"Allow uagent?" in output, output)
        assert_true(b"$CODEX_LOCAL_PROXY_API_KEY" in output, output)
        assert_true(b"adjacent-integration-secret" not in output, output)
        # Status redraws may insert cursor controls before the colored line.
        assert_true(b'\x1b[32m+   "codex-local": {' in output, output)
        # The diff belongs to the approval prompt alone, never the call label.
        assert_true(output.count(b'+   "codex-local": {') == 1, output)
        written = config.read_text()
        assert_true(proposed in written, written)
        assert_true("# keep me" in written, written)


def test_composite_configuration_rejects_literal_credentials(root, home, *, binary):
    """A literal credential is rejected without a prompt or terminal leak."""
    config = home / ".uagent" / ".config"
    config.parent.mkdir(parents=True, exist_ok=True)
    original = "# unchanged\n"
    config.write_text(original)
    literal = "literal-provider-secret"
    proposed = json.dumps(
        {
            "bad": {
                "base_url": "https://example.com/v1",
                "api_key": literal,
            }
        },
        separators=(",", ":"),
    )

    def request_change(_, __):
        return tool_call(
            "uagent",
            {
                "action": "configure",
                "scope": "user",
                "changes": [{"key": "UAGENT_PROVIDERS", "operation": "set", "value": proposed}],
            },
        )

    def finish(_, body):
        result = tool_results(body["messages"])[-1]
        assert_true("environment-variable reference" in result, result)
        assert_true(literal not in result, result)
        return event({"content": "literal-config-rejected"})

    with Server([request_change, finish]) as server:
        status, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"configure providers\n", b"literal-config-rejected"),
                b"/quit\n",
            ],
            args=("--yolo",),
            timeout=20,
            binary=binary,
        )
        assert_true(status == 0, output)
        assert_true(b"Allow uagent?" not in output, output)
        assert_true(literal.encode() not in output, output)
        assert_true(config.read_text() == original, config.read_text())


def test_self_configuration_requires_a_person(root, home, *, binary):
    """With nobody to ask, the tool is not offered and the file is untouched.

    The approver denies a mandatory-human call whenever no interactive
    terminal is attached, so advertising the schema to a piped run would spend
    a kilobyte on every request to buy a guaranteed refusal. Withholding it is
    the same policy stated earlier: registration and approval share one
    predicate.
    """
    config = home / ".uagent" / ".config"
    config.parent.mkdir(parents=True, exist_ok=True)
    original = "# keep me\nUAGENT_MAX_TOOL_CALLS=40\n"
    config.write_text(original)

    def refuse(_, body):
        names = function_names(body)
        assert_true("uagent" in names, names)
        tool = next(t["function"] for t in body["tools"] if t["function"]["name"] == "uagent")
        assert_true(tool["parameters"]["properties"]["action"]["enum"] == ["inspect"], tool)
        # The escape hatch is closed too: writing the file directly stays a
        # mandatory-human mutation.
        assert_true("write_file" in names, names)
        return event({"content": "configure-absent"})

    with Server([refuse]) as server:
        result = run(
            root, base_env(home, server.url), "--yolo", "-p", "raise the limit", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("configure-absent"), result.stdout)
        assert_true(config.read_text() == original, config.read_text())


def test_self_configuration_commits_after_approval(root, home, *, binary):
    """An approved change preserves comments and reports when it takes effect."""
    config = home / ".uagent" / ".config"
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text("# keep me\nUAGENT_MAX_TOOL_CALLS=40\nUNKNOWN_KEY=kept\n")

    def request_change(_, __):
        return tool_call(
            "uagent",
            {
                "action": "configure",
                "scope": "user",
                "changes": [{"key": "UAGENT_MAX_TOOL_CALLS", "operation": "set", "value": "120"}],
            },
        )

    def finish(_, body):
        result = tool_results(body["messages"])[-1]
        assert_true("wrote" in result, result)
        assert_true("active at the next user turn" in result, result)
        return event({"content": "configure-ok"})

    with Server([request_change, finish]) as server:
        env = base_env(home, server.url)
        # A real terminal: piped stdin is deliberately not treated as a person.
        # The answer is sent only once the approval prompt is on screen, so it
        # cannot be swallowed as steering while the turn is still working.
        status, output = run_pty(
            root,
            env,
            [
                (b"raise the limit\n", b"Allow uagent?"),
                (b"y\n", b"configure-ok"),
                b"/quit\n",
            ],
            timeout=20,
            binary=binary,
        )
        assert_true(status == 0, output)
        assert_true(b"configure-ok" in output, output)
        assert_true(b"wrote " in output, output)
        written = config.read_text()
        assert_true("UAGENT_MAX_TOOL_CALLS=120" in written, written)
        assert_true("# keep me" in written, written)
        assert_true("UNKNOWN_KEY=kept" in written, written)


def test_approval_remembers_exact_action_and_forwards_a_refusal(root, home, *, binary):
    """An exact action is remembered; a different command still asks."""
    refusal = {}

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "use git log instead"):
            refusal["steered"] = True
            return event({"content": "approval-done"})
        results = tool_results(messages)
        if not results:
            return tool_call("run", {"command": "git status --short"})
        if len(results) == 1:
            return tool_call("run", {"command": "git status --short"})
        return tool_call("run", {"command": "rm -rf /tmp/uagent-nothing"})

    with Server([route]) as server:
        status, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"go\n", b"Allow for this session"),
                (
                    b"s\n",
                    b"rm -rf /tmp/uagent-nothing",
                    b"Allow for this session",
                    None,
                ),
                (b"use git log instead\n", b"approval-done"),
                b"/quit\n",
            ],
            timeout=25,
            binary=binary,
        )
        assert_true(status == 0, output)
        assert_true(b"[y] Allow once" in output and b"[n] Deny" in output, output)
        # Reaching rm proves the exact repeat ran without consuming the refusal;
        # rm is a different command, so that refusal is delivered there.
        assert_true(b"Running rm -rf /tmp/uagent-nothing" in output, output)
        # The refusal reached the model as guidance rather than a bare denial.
        assert_true(refusal.get("steered"), (refusal, output))
        assert_true(b"approval-done" in output, output)
