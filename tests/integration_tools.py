from integration_support import (
    SMALL_PNG,
    Server,
    assert_true,
    base_env,
    captured_log_path,
    detached_pid,
    event,
    function_names,
    json,
    json_sentinel_command,
    large_json_command,
    os,
    pathlib,
    run,
    run_dialog,
    run_pty,
    shlex,
    signal,
    signal_process_group,
    time,
    tool_call,
    tool_results,
)


def test_attach_tool_puts_bytes_in_context(root, home):
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
        return event(  # both in one batch: attach is parallel_safe
            {
                "tool_calls": [
                    {
                        "index": i,
                        "id": f"call-{i}",
                        "function": {
                            "name": "attach",
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
            workspace,
            env,
            "--yolo",
            f"--debug={trace}",
            "-p",
            "look",
            timeout=40,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "attach-ok", result.stdout)
        assert_true(seen.get("image"), seen)
        assert_true(seen.get("file"), seen)
        assert_true(len(server.requests) == 2, len(server.requests))
        events = [json.loads(line) for line in trace.read_text().splitlines()]
        assert_true(not any(event["event"] == "midturn_compact" for event in events), events)
        # the base64 payload must not survive into the saved session
        sessions = list((home / ".uagent" / "history").rglob("*.json"))
        blobs = [
            s for s in sessions if "base64," in s.read_text(encoding="utf-8", errors="replace")
        ]
        assert_true(not blobs, blobs)


def test_full_run_and_python_terminal_trace(root, home):
    shell_command = "printf 'shell-one\\n'\nprintf 'shell-two\\n'"
    python_code = "print('python-one')\nprint('python-two')"
    with Server(
        [
            tool_call("run", {"command": shell_command, "shell": "/bin/sh"}),
            tool_call(
                "scratch",
                {"path": "trace.py", "code": python_code, "packages": []},
            ),
            event({"content": "trace-ok"}),
        ]
    ) as server:
        env = base_env(home, server.url)
        env["UAGENT_TOOL_BATCH_RESULT_CHARS"] = "8"
        result = run_dialog(
            root,
            env,
            "/verbose\ntrace\n/trace\n/q\n",
            "--yolo",
            timeout=20,
        )
        assert_true(result.returncode == 0, result.stderr)
        for expected in (
            "printf 'shell-one",
            "printf 'shell-two",
            "shell-one",
            "shell-two",
            "scratch(write/replace trace.py → execute)",
            "[script: .uagent/scratch/trace.py · wrote · executed]",
            "python-one",
            "python-two",
            "latest trace · turn 1 · 2 tools",
            "→ [1] run",
            "← [2] scratch",
            "trace-ok",
        ):
            assert_true(expected in result.stdout, result.stdout)


def test_large_run_output_is_recoverable(root, home):
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
        result = run(root, env, "--yolo", "-p", "inspect large output")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "artifact-ok", result.stdout)
        assert_true(artifact["path"].exists(), artifact)


def test_run_rejects_python_and_sudo_before_execution(root, home):
    def after_python(_, body):
        results = tool_results(body["messages"])
        assert_true(any("use scratch" in value for value in results), results)
        return tool_call("run", {"command": "sudo true"})

    def after_sudo(_, body):
        results = tool_results(body["messages"])
        assert_true(
            any("privileged commands are unavailable" in value for value in results),
            results,
        )
        return event({"content": "guarded"})

    with Server(
        [
            tool_call("run", {"command": "python -c 'print(1)'"}),
            after_python,
            after_sudo,
        ]
    ) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "work")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "guarded", result.stdout)


def test_grep_tool_round_trip(root, home):
    workspace = root / "grep-workspace"
    workspace.mkdir()
    (workspace / "one.cpp").write_text("alpha\nproject_wide_symbol\nomega\n", encoding="utf-8")
    (workspace / "ignored.txt").write_text("project_wide_symbol\n", encoding="utf-8")

    def final(_, body):
        result = tool_results(body["messages"])[0]
        valid = (
            "one.cpp" in result
            and "ignored.txt" not in result
            and "project_wide_symbol" in result
            and "alpha" in result
            and "omega" in result
        )
        return event({"content": "grep-ok" if valid else "grep-bad"})

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
            final,
        ]
    ) as server:
        env = base_env(home, server.url)
        env["UAGENT_IMAGE_PROTOCOL"] = "iterm"
        result = run(workspace, env, "-p", "search")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "grep-ok", result.stdout)
        names = function_names(server.requests[0][1])
        assert_true("grep" in names, names)
        assert_true("show_image" not in names, names)


def test_skill_tool_offers_and_opens(root, home):
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
            [b"reply\n", b"/trace\n", b"\x04"],
            columns=24,
        )
        assert_true(code == 0, output)
        assert_true(b"skill-ok" in output and b"Skills" in output, output)
        assert_true(b"1 available" in output, output)
        assert_true(b"\x1b[1m\xc2\xb5Agent" in output, output)
        assert_true(b"demo" in output, output)
        assert_true(
            output.find(b"demo-body-sentinel") > output.find(b"skill-ok"),
            output,
        )


def test_tool_trace_repeated_rounds_are_telemetry_only(root, home):
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
        env["UAGENT_AUTO_COMPACT_TOKENS"] = "0"
        result = run(root, env, "--yolo", f"--debug={trace}", "-p", "inspect lines")
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("rounds-finished"), result.stdout)
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        signals = [r for r in records if r["event"] == "repeated_tool_rounds"]
        assert_true(len(signals) == 1, signals)
        assert_true(signals[0]["data"]["tool"] == "read_path", signals)
        assert_true(signals[0]["data"]["rounds"] == 8, signals)


def test_invalid_tool_rejection_loop_stops_before_fourth_round(root, home):
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
            root, base_env(home, server.url), "--yolo", f"--debug={trace}", "-p", "inspect"
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


def test_detached_terminal_materialized_wait_does_not_bypass_repeat_guard(root, home):
    call = {"operation": "list", "wait_ms": 1}
    responses = [tool_call("activity", call) for _ in range(4)]
    responses.append(event({"content": "fifth-round-should-not-run"}))
    with Server(responses) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "list")
        assert_true(result.returncode != 0, result.stdout)
        assert_true(
            "model repeated the same tool call more than 3 times" in result.stderr,
            result.stderr,
        )
        assert_true(len(server.requests) == 4, len(server.requests))


def test_parallel_run_overlaps(root, home):
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
        result = run(root, base_env(home, server.url), "--yolo", "-p", "go", timeout=90)
        elapsed = time.time() - started
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "parallel-run-ok", result.stdout)
        # serial would be count*sleep; allow generous slack for spawn overhead
        assert_true(elapsed < sleep * count * 0.7, f"{elapsed:.1f}s for {count}x{sleep}s")


def test_detached_terminal_survives_and_is_readable(root, home):
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
            launch_env = base_env(home, launch_server.url)
            launch_env["UAGENT_BASH_LOG_BYTES"] = "4096"
            launched = run_dialog(
                workspace,
                launch_env,
                "launch the server\n/ps\n/q\n",
                "--yolo",
                timeout=8,
            )
            for _ in range(40):
                if pid_file.exists():
                    break
                time.sleep(0.05)
            assert_true(pid_file.exists(), "detached command did not start")
            pid = int(pid_file.read_text(encoding="utf-8"))
            assert_true(launched.returncode == 0, launched.stderr)
            assert_true("launched" in launched.stdout, launched.stdout)
            assert_true("bg:1" in launched.stdout, launched.stdout)
            assert_true("background work" in launched.stdout, launched.stdout)
            assert_true("[detached] activity" in launched.stdout, launched.stdout)
            os.kill(pid, 0)

        with Server([event({"content": "fresh"})]) as fresh_server:
            fresh = run_dialog(
                workspace,
                base_env(home, fresh_server.url),
                "status\n/q\n",
                timeout=8,
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
            )
            assert_true(result.returncode == 0, result.stderr)
            assert_true(result.stdout.strip() == "terminal-ok", result.stdout)
            os.kill(pid, 0)
            record = home / ".uagent" / "terminals" / f"{pid}.json"
            assert_true(record.exists(), record)
            record_data = json.loads(record.read_text(encoding="utf-8"))
            log = pathlib.Path(record_data["log"])
            log_bytes = sum(
                path.stat().st_size
                for path in (log, pathlib.Path(str(log) + ".1"))
                if path.exists()
            )
            assert_true(log_bytes <= 4096, log_bytes)
            assert_true(pathlib.Path(str(log) + ".1").exists(), log)

        def verify_stop(_, body):
            result = tool_results(body["messages"])[-1]
            return event(
                {"content": "terminal-stop-ok" if "stopped process group" in result else result}
            )

        with Server([tool_call("activity_stop", {"id": pid}), verify_stop]) as stop_server:
            stop_env = base_env(home, stop_server.url)
            stopped = run(
                workspace,
                stop_env,
                "--yolo",
                "-p",
                "stop the detached server",
                timeout=8,
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
                raise AssertionError(f"detached process group {pid} survived activity_stop")
            pid = None
    finally:
        signal_process_group(pid)


def test_detached_terminal_tracks_group_after_wrapper_exit(root, home):
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

    def inspect_group(_, body):
        return tool_call("activity", {"operation": "poll", "id": state["pid"]})

    def stop_group(_, body):
        result = tool_results(body["messages"])[-1]
        os.kill(state["child"], 0)
        tracked = result.startswith(f"[detached · activity {state['pid']} ")
        return (
            tool_call("activity_stop", {"id": state["pid"]})
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
        result = run(root, env, "--yolo", "-p", "manage server", timeout=10)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(result.stdout.strip() == "group-tracking-ok", result.stdout)
    finally:
        server.close()
        signal_process_group(state["pid"], signal.SIGKILL)


TESTS = (
    test_attach_tool_puts_bytes_in_context,
    test_full_run_and_python_terminal_trace,
    test_large_run_output_is_recoverable,
    test_run_rejects_python_and_sudo_before_execution,
    test_grep_tool_round_trip,
    test_skill_tool_offers_and_opens,
    test_tool_trace_repeated_rounds_are_telemetry_only,
    test_invalid_tool_rejection_loop_stops_before_fourth_round,
    test_detached_terminal_materialized_wait_does_not_bypass_repeat_guard,
    test_parallel_run_overlaps,
    test_detached_terminal_survives_and_is_readable,
    test_detached_terminal_tracks_group_after_wrapper_exit,
)
