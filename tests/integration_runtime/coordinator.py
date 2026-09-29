import json
import pathlib
import threading
import time

from integration_support import (
    Server,
    assert_true,
    base_env,
    budget,
    event,
    function_names,
    run,
    run_pty,
    session_files,
    tool_call,
    tool_calls,
    tool_results,
    wait_until,
    write_session,
)

COORDINATOR_TOOLS = {
    "read_path",
    "grep",
    "memory",
    "skill",
    "uagent",
    "history",
    "thread",
    "decide",
    "ask",
    "state",
}


def test_coordinator_is_one_read_only_session_per_folder(root, home, *, binary):
    with Server([event({"content": "first-ok"}), event({"content": "second-ok"})]) as server:
        env = base_env(home, server.url)
        # Widening capabilities never widens the coordinator.
        env["UAGENT_TOOL_CAPABILITIES"] = "inspect,execute,mutate,delegate,external"
        for text, answer in ((b"first\n", b"first-ok"), (b"second\n", b"second-ok")):
            code, output = run_pty(
                root,
                env,
                [(text, answer, b"Ready", None), b"/q\n"],
                args=("coord",),
                binary=binary,
            )
            assert_true(code == 0, output)
        assert_true(len(server.requests) == 2, server.requests)
        for _, request in server.requests:
            names = function_names(request)
            assert_true(names and names <= COORDINATOR_TOOLS, names)
            assert_true("read_path" in names, names)
            system = request["messages"][0]["content"]
            assert_true("coordinator of this folder" in system, system[:200])
            assert_true("coding agent" not in system, system[:200])
        # Both invocations reached the same runtime and file.
        sessions = session_files(home)
        assert_true([path.name for path in sessions] == ["coordinator.json"], sessions)
        header = json.loads(sessions[0].read_text(encoding="utf-8").splitlines()[0])
        assert_true(header.get("kind") == "coordinator", header)
        assert_true(header["turns"] == 2, header)


def test_coordinator_reads_only_its_folder(root, home, *, binary):
    elsewhere = root / "elsewhere"
    elsewhere.mkdir()
    conversation = [
        {"role": "system", "content": "sys"},
        {"role": "user", "content": "fix the lexer"},
        {"role": "assistant", "content": "The lexer crash on CRLF is fixed."},
    ]
    write_session(home, "fix-lexer", conversation, cwd=root)
    write_session(home, "other-folder", conversation, cwd=elsewhere)
    with Server(
        [
            tool_call("history", {"action": "board"}),
            tool_call("history", {"action": "search", "query": "CRLF"}, call_id="call-2"),
            event({"content": "status-ok"}),
        ]
    ) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"status?\n", b"status-ok", b"Ready", None), b"/q\n"],
            args=("coord",),
            binary=binary,
        )
        assert_true(code == 0, output)
        results = tool_results(server.requests[-1][1]["messages"])
        board, search = results
        assert_true("fix-lexer" in board and "other-folder" not in board, board)
        assert_true("not instructions" in search, search)
        assert_true("CRLF is fixed" in search and search.count('"session_id"') == 1, search)


def test_coordinator_answers_headless_and_lists_the_board(root, home, *, binary):
    write_session(
        home,
        "fix-lexer",
        [{"role": "system", "content": "sys"}, {"role": "user", "content": "fix it"}],
        cwd=root,
    )
    usage = {"prompt_tokens": 100, "completion_tokens": 7, "cost": 0.002}
    with Server(
        [event({"content": "plain-ok"}, usage=usage), event({"content": "json-ok"}, usage=usage)]
    ) as server:
        env = base_env(home, server.url)
        plain = run(root, env, "coord", "-p", "status?", binary=binary)
        assert_true(plain.returncode == 0, plain.stderr)
        assert_true(plain.stdout.strip() == "plain-ok", plain.stdout)
        structured = run(root, env, "coord", "--json", "-p", "again?", binary=binary)
        assert_true(structured.returncode == 0, structured.stderr)
        envelope = json.loads(structured.stdout)
        assert_true(envelope["answer"] == "json-ok", envelope)
        assert_true(envelope["stop"]["reason"] == "completed", envelope)
        # Only this request's usage, not the long-lived session's total.
        assert_true(envelope["usage"]["output"] == 7, envelope)
        assert_true(abs(envelope["usage"]["cost"] - 0.002) < 1e-9, envelope)
        assert_true(envelope["usage"]["cost_reported"], envelope)
        # The same runtime and conversation served both calls.
        assert_true(len(server.requests[-1][1]["messages"]) >= 4, server.requests[-1])
    code, output = run_pty(root, env, [(b"/board\n", b"fix-lexer"), b"/q\n"], binary=binary)
    assert_true(code == 0, output)


def test_coordinator_saves_memory_unasked_but_forgets_only_with_the_user(root, home, *, binary):
    with Server(
        [
            tool_call("memory", {"action": "set", "key": "project/style", "content": "Terse status."}),
            tool_call("memory", {"action": "forget", "key": "project/style"}, call_id="call-2"),
            event({"content": "noted-ok"}),
        ]
    ) as server:
        result = run(root, base_env(home, server.url), "coord", "-p", "be terse", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        saved, forgot = tool_results(server.requests[-1][1]["messages"])
        assert_true("approval" not in saved.lower() and "error" not in saved.lower(), saved)
        # Headless has no one to ask, so the forget is refused.
        assert_true("approval" in forgot.lower() or "denied" in forgot.lower(), forgot)
        system = server.requests[0][1]["messages"][0]["content"]
        assert_true("Noted:" in system, system)


def test_coordinator_delegates_a_thread_and_hears_back(root, home, *, binary):
    heard = threading.Event()

    def route(_, body):
        text = json.dumps(body["messages"])
        if "[thread event" in text:
            heard.set()
            return event({"content": "noted-event"})
        if "Objective: count the files" in text:
            return event({"content": "thread-report: 3 files"})
        if not tool_results(body["messages"]):
            return tool_call(
                "thread",
                {
                    "action": "spawn",
                    "title": "Count files",
                    "objective": "count the files",
                    "environment": "local",
                },
            )
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "coord", "-p", "delegate a count", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "spawned-ok", result.stdout)
        spawned = json.loads(
            next(
                tool_results(body["messages"])[0]
                for _, body in server.requests
                if "delegate a count" in json.dumps(body) and tool_results(body["messages"])
            )
        )
        assert_true(len(spawned["session_id"]) == 16, spawned)
        # An idle coordinator batches thread events for up to 20 seconds.
        assert_true(heard.wait(budget(40)), [json.dumps(b)[-300:] for _, b in server.requests])
        threads = [
            path
            for path in session_files(home)
            if path.name.startswith("thread-")
        ]
        assert_true(len(threads) == 1, threads)
        header = json.loads(threads[0].read_text(encoding="utf-8").splitlines()[0])
        assert_true(header["kind"] == "thread", header)
        link = header["thread"]
        assert_true(link["folder"] == str(root.resolve()), link)
        assert_true("budget_usd" in link["ceiling"], link)
        # The thread's own requests carried only its brief, never the coordinator's.
        thread_requests = [
            body for _, body in server.requests if "Objective: count the files" in json.dumps(body)
        ]
        assert_true(thread_requests, server.requests)
        assert_true("history" not in function_names(thread_requests[0]), thread_requests[0])


def test_coordinator_refuses_spawns_past_its_spend_limit(root, home, *, binary):
    from integration_support import fnv1a64

    coordinator = fnv1a64(str(home / ".uagent" / "history" / fnv1a64(str(root.resolve())) / "coordinator.json"))
    write_session(
        home,
        "thread-spent",
        [{"role": "system", "content": "sys"}],
        cwd=root,
        usage={"cost": 1.5, "cost_reported": True},
        kind="thread",
        thread={
            "coordinator_id": coordinator,
            "folder": str(root.resolve()),
            "day": time.strftime("%Y-%m-%d"),
        },
    )
    with Server(
        [
            tool_call(
                "thread",
                {"action": "spawn", "title": "More", "objective": "more", "environment": "local"},
            ),
            event({"content": "refused-ok"}),
        ]
    ) as server:
        env = base_env(home, server.url)
        env["UAGENT_COORDINATOR_DAILY_SPEND_USD"] = "1"
        result = run(root, env, "coord", "-p", "more", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        refusal = tool_results(server.requests[-1][1]["messages"])[0]
        assert_true("spend limit" in refusal, refusal)


def test_restarted_threads_keep_their_ceiling_and_user_sessions_stay_asleep(root, home, *, binary):
    from integration_support import fnv1a64

    coordinator = fnv1a64(str(home / ".uagent" / "history" / fnv1a64(str(root.resolve())) / "coordinator.json"))
    conversation = [{"role": "system", "content": "sys"}, {"role": "user", "content": "go"}]
    write_session(
        home,
        "thread-over",
        conversation,
        cwd=root,
        usage={"cost": 5, "cost_reported": True},
        kind="thread",
        thread={"coordinator_id": coordinator, "folder": str(root.resolve()), "ceiling": {"budget_usd": 1}},
    )
    write_session(home, "mine", conversation, cwd=root)
    ids = {path.stem: fnv1a64(str(path)) for path in session_files(home)}
    with Server(
        [
            tool_call("thread", {"action": "message", "session_id": ids["thread-over"], "text": "more"}),
            tool_call("thread", {"action": "message", "session_id": ids["mine"], "text": "more"}, call_id="call-2"),
            event({"content": "done-ok"}),
        ]
    ) as server:
        result = run(root, base_env(home, server.url), "coord", "-p", "nudge", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        sent, refused = tool_results(server.requests[-1][1]["messages"])
        assert_true(sent == "sent", sent)
        assert_true("not running" in refused, refused)
        time.sleep(budget(1))
        # The restarted thread stopped at its budget before asking the model.
        assert_true(len(server.requests) == 3, [json.dumps(b)[-200:] for _, b in server.requests])


def test_coordinator_messages_a_thread_at_most_three_times_in_a_row(root, home, *, binary):
    from integration_support import fnv1a64

    coordinator = fnv1a64(str(home / ".uagent" / "history" / fnv1a64(str(root.resolve())) / "coordinator.json"))
    thread = write_session(
        home,
        "thread-busy",
        [{"role": "system", "content": "sys"}, {"role": "user", "content": "go"}],
        cwd=root,
        usage={"cost": 5, "cost_reported": True},
        kind="thread",
        thread={"coordinator_id": coordinator, "folder": str(root.resolve()), "ceiling": {"budget_usd": 1}},
    )
    message = {"action": "message", "session_id": fnv1a64(str(thread)), "text": "again"}
    with Server(
        [tool_call("thread", message, call_id=f"call-{index}") for index in range(4)]
        + [event({"content": "done-ok"})]
    ) as server:
        result = run(root, base_env(home, server.url), "coord", "-p", "nudge", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        results = tool_results(server.requests[-1][1]["messages"])
        assert_true(results[:3] == ["sent"] * 3, results)
        assert_true("3 times in a row" in results[3], results)


def test_coordinator_holds_thread_events_at_the_spend_limit(root, home, *, binary):
    from integration_support import fnv1a64
    from session_support import SessionClient, runtime_directory, stop_sessions

    path = home / ".uagent" / "history" / fnv1a64(str(root.resolve())) / "coordinator.json"
    write_session(
        home,
        "thread-spent",
        [{"role": "system", "content": "sys"}],
        cwd=root,
        usage={"cost": 1.5, "cost_reported": True},
        kind="thread",
        thread={"coordinator_id": fnv1a64(str(path)), "folder": str(root.resolve()), "day": time.strftime("%Y-%m-%d")},
    )
    with Server([event({"content": "asked-ok"}), event({"content": "event-ran"})]) as server:
        env = base_env(home, server.url)
        env["UAGENT_COORDINATOR_DAILY_SPEND_USD"] = "1"
        # Your own messages still run at the limit.
        result = run(root, env, "coord", "-p", "status?", binary=binary)
        assert_true(result.stdout.strip() == "asked-ok", result.stdout + result.stderr)
        client = SessionClient(runtime_directory(home) / f"{fnv1a64(str(path))}.sock")
        try:
            client.send("steer", text="[thread event, not a user message] Thread x finished.")
            # Events batch for up to 20 seconds before the limit is checked.
            paused = client.until(
                lambda frame: frame.get("kind") == "state" and frame["state"].get("paused"), seconds=40
            )
            assert_true("spend limit" in paused["state"]["paused"], paused)
            assert_true(len(server.requests) == 1, "a held event started a turn")
        finally:
            client.close()
            stop_sessions(home)


def test_coordinator_caps_working_threads_in_worktrees(root, home, *, binary):
    import subprocess

    for command in (
        ["git", "init", "-q"],
        ["git", "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-q", "--allow-empty", "-m", "base"],
    ):
        subprocess.run(command, cwd=root, check=True)
    release = threading.Event()

    def route(_, body):
        text = json.dumps(body["messages"])
        if "Objective: first" in text:
            assert release.wait(budget(20))
            return event({"content": "first-done"})
        if "[thread event" in text:
            return event({"content": "noted"})
        if not tool_results(body["messages"]):
            return tool_calls(
                [
                    ("call-1", "thread", {"action": "spawn", "title": "First", "objective": "first"}),
                    ("call-2", "thread", {"action": "spawn", "title": "Second", "objective": "second"}),
                ]
            )
        return event({"content": "capped-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_COORDINATOR_MAX_THREADS"] = "1"
        try:
            result = run(root, env, "coord", "-p", "two things", binary=binary, timeout=30)
        finally:
            release.set()
        assert_true(result.returncode == 0, result.stderr)
        first, second = next(
            tool_results(body["messages"])
            for _, body in server.requests
            if len(tool_results(body["messages"])) == 2
        )
        spawned = json.loads(first)
        worktree = pathlib.Path(spawned["cwd"])
        assert_true(spawned["environment"] == "worktree", spawned)
        assert_true(worktree.parent.name == "worktrees" and worktree != root, spawned)
        assert_true("already working" in second, second)


def test_coordinator_deletes_only_with_the_user(root, home, *, binary):
    victim = write_session(
        home, "keep-me", [{"role": "system", "content": "sys"}], cwd=root
    )
    session_id = __import__("integration_support").fnv1a64(str(victim))
    with Server(
        [
            tool_call("thread", {"action": "delete", "session_id": session_id}),
            event({"content": "delete-ok"}),
        ]
    ) as server:
        env = base_env(home, server.url)
        env["UAGENT_APPROVAL"] = "yolo"
        result = run(root, env, "coord", "-p", "delete it", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("declined" in result.stderr, result.stderr)
        assert_true(victim.exists(), "deleted without the user")


def _thread_session(home):
    threads = [path for path in session_files(home) if path.name.startswith("thread-")]
    assert_true(len(threads) == 1, threads)
    return threads[0]


def _spawn_then(decide):
    """A router: the coordinator spawns one local thread that writes a file,
    and answers the routed approval with `decide(thread_id, interaction_id)`."""
    import re

    state = {"asked": threading.Event(), "done": threading.Event()}

    def route(_, body):
        text = json.dumps(body["messages"])
        results = tool_results(body["messages"])
        if "Objective: write out.txt" in text:
            if not results:
                return tool_call("write_file", {"path": "out.txt", "content": "x"})
            state["done"].set()
            return event({"content": "thread-done"})
        if "[approval request" in text and "sent " not in json.dumps(results):
            match = re.search(r"Thread ([0-9a-f]{16}) .*?interaction ([^)]+)\)", text)
            state["asked"].set()
            return tool_call("decide", decide(match.group(1), match.group(2)))
        if "[approval request" in text or "[thread event" in text:
            return event({"content": "coordinator-ack"})
        if not results:
            return tool_call(
                "thread",
                {
                    "action": "spawn",
                    "title": "Write",
                    "objective": "write out.txt",
                    "environment": "local",
                },
            )
        return event({"content": "spawned-ok"})

    return route, state


def test_coordinator_approves_what_auto_could_not(root, home, *, binary):
    route, state = _spawn_then(
        lambda thread, interaction: {
            "session_id": thread,
            "interaction_id": interaction,
            "decision": "allow_once",
            "reason": "inside the brief",
        }
    )
    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "coord", "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(state["asked"].wait(budget(20)), "approval never reached the coordinator")
        assert_true(state["done"].wait(budget(20)), "thread never continued")
        assert_true((root / "out.txt").read_text() == "x", "write was not allowed")
        journal = pathlib.Path(str(_thread_session(home)) + ".events.jsonl")
        wait_until(
            lambda: "coordinator decided y: inside the brief" in journal.read_text(),
            "the decision is missing from the thread's log",
        )


def test_yielded_and_mandatory_decisions_reach_the_user(root, home, *, binary):
    from session_support import SessionClient, runtime_directory

    route, state = _spawn_then(
        lambda thread, interaction: {
            "session_id": thread,
            "interaction_id": interaction,
            "decision": "yield",
            "reason": "writes outside the brief",
        }
    )
    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "coord", "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(state["asked"].wait(budget(20)), "approval never reached the coordinator")
        thread = _thread_session(home)
        socket = runtime_directory(home) / f"{__import__('integration_support').fnv1a64(str(thread))}.sock"
        client = SessionClient(socket)
        try:
            yielded = client.until(
                lambda frame: frame.get("kind") == "state"
                and (frame.get("pending") or {}).get("route") == "human"
            )
            assert_true(yielded["pending"]["note"] == "writes outside the brief", yielded)
            # The user answers the yielded decision as any other.
            client.send("reply", interaction_id=yielded["pending"]["id"], text="y")
            assert_true(state["done"].wait(budget(20)), "thread never continued")
        finally:
            client.close()
        assert_true((root / "out.txt").read_text() == "x", "write was not allowed")


def test_mandatory_decisions_skip_the_coordinator(root, home, *, binary):
    from session_support import SessionClient, runtime_directory

    def route(_, body):
        text = json.dumps(body["messages"])
        assert "[approval request" not in text, "a mandatory decision reached the coordinator"
        if "Objective: run it" in text:
            return tool_call("run", {"command": "true", "sandbox": False})
        if "[thread event" in text:
            return event({"content": "ack"})
        if not tool_results(body["messages"]):
            return tool_call(
                "thread",
                {"action": "spawn", "title": "Run", "objective": "run it", "environment": "local"},
            )
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "coord", "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        thread = _thread_session(home)
        fnv = __import__("integration_support").fnv1a64
        client = SessionClient(runtime_directory(home) / f"{fnv(str(thread))}.sock")
        try:
            waiting = client.until(
                lambda frame: frame.get("kind") == "state" and frame.get("pending")
            )
            pending = waiting["pending"]
            assert_true(pending["approval"]["mandatory_human"], pending)
            assert_true("route" not in pending, pending)
            client.send("reply", interaction_id=pending["id"], text="n")
        finally:
            client.close()


def test_coordinator_context_carries_instructions_notes_board_and_time(root, home, *, binary):
    import re

    instructions = home / ".uagent" / "COORDINATOR.md"
    instructions.parent.mkdir(parents=True, exist_ok=True)
    instructions.write_text("Prefer small threads.")
    with Server(
        [
            tool_call("state", {"action": "set", "block": "goals", "text": "ship v1"}),
            tool_call(
                "uagent",
                {
                    "action": "set_instructions",
                    "audience": "coordinator",
                    "scope": "user",
                    "text": "Be reckless.",
                },
                call_id="call-2",
            ),
            event({"content": "noted"}),
            event({"content": "second"}),
        ]
    ) as server:
        env = base_env(home, server.url)
        first = run(root, env, "coord", "-p", "remember the goal", binary=binary)
        assert_true(first.returncode == 0, first.stderr)
        assert_true("declined" in first.stderr, first.stderr)
        assert_true(
            instructions.read_text() == "Prefer small threads.",
            "instructions changed without the user",
        )
        second = run(root, env, "coord", "-p", "what now?", binary=binary)
        assert_true(second.returncode == 0, second.stderr)
        messages = server.requests[-1][1]["messages"]
        system = messages[0]["content"]
        assert_true("Prefer small threads." in system, system[-300:])
        context = json.dumps(messages)
        assert_true("## goals\\nship v1" in context and "## board" in context, context[-800:])
        # Rebuilt per request, never stored: rewriting stored history would
        # move display rows and spoil the provider's cached prefix.
        stored = next(p for p in session_files(home) if p.name == "coordinator.json")
        assert_true("## board" not in stored.read_text(), "coordinator context was stored")
        # The typed text is stored as typed; only the request carries stamps.
        stored_messages = json.loads(stored.read_text().split("\n", 1)[1])["messages"]
        assert_true(
            any(m.get("content") == "what now?" for m in stored_messages), stored_messages
        )
        assert_true(messages[-1]["role"] == "user" and "## board" in messages[-1]["content"], messages[-1])
        users = [m["content"] for m in messages if m["role"] == "user"]
        assert_true(
            any(re.match(r"\[\w{3} \d\d \w{3} \d\d:\d\d \S+\] what now\?", u) for u in users),
            users,
        )
    # The user sees the instructions from any terminal session.
    code, output = run_pty(
        root, env, [(b"/instructions\n", b"Prefer small threads."), b"/q\n"], binary=binary
    )
    assert_true(code == 0, output)


def test_only_checkpoints_carry_the_view(root, home, *, binary):
    from integration_support import fnv1a64
    from session_support import SessionClient, runtime_directory, stop_sessions

    path = home / ".uagent" / "history" / fnv1a64(str(root.resolve())) / "coordinator.json"
    with Server([event({"content": "first"}), event({"content": "second"})]) as server:
        result = run(root, base_env(home, server.url), "coord", "-p", "hi", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        client = SessionClient(runtime_directory(home) / f"{fnv1a64(str(path))}.sock")
        try:
            sent = client.send("submit", text="again")
            client.until(
                lambda frame: frame.get("kind") == "state"
                and frame.get("completed_request_id") == sent["request_id"]
            )
            # After the catch-up snapshot sent on connect.
            states = [frame for frame in client.frames if frame.get("kind") == "state"][1:]
            light = [frame for frame in states if not frame.get("checkpoint")]
            assert_true(light, states)
            for frame in light:
                # Clients keep the last checkpoint's view and apply block events.
                for field in ("view", "http", "system_prompt"):
                    assert_true(field not in frame["state"], (field, list(frame["state"])))
            assert_true("view" in states[-1]["state"], list(states[-1]["state"]))
        finally:
            client.close()
            stop_sessions(home)
