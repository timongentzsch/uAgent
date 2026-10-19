import json
import pathlib
import re
import subprocess
import threading
import time

from integration_support import (
    Server,
    assert_true,
    base_env,
    budget,
    event,
    fnv1a64,
    function_names,
    run,
    run_dialog,
    run_pty,
    save_settings,
    session_files,
    tool_call,
    tool_calls,
    tool_results,
    wait_until,
    write_session,
)
from session_support import SessionClient, runtime_directory, stop_sessions


def _spawn(title, objective, *, environment="local"):
    """A thread spawn's arguments; `environment=None` leaves it to the setting."""
    spawn = {"action": "spawn", "title": title, "objective": objective}
    return spawn | ({"environment": environment} if environment else {})


def _panel_path(home, root):
    """Where the panel of `root` keeps its session."""
    return home / ".uagent" / "history" / fnv1a64(str(root.resolve())) / "coordinator.json"


def _socket(home, session):
    """The runtime socket of the saved session at `session`."""
    return runtime_directory(home) / f"{fnv1a64(str(session))}.sock"


PANEL_TOOLS = {
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
    "browser",
    "artifact",
}


def test_panel_is_one_read_only_session_per_folder(root, home, *, binary):
    with Server([event({"content": "first-ok"}), event({"content": "second-ok"})]) as server:
        env = base_env(home, server.url)
        # Widening capabilities never widens the panel.
        env["UAGENT_TOOL_CAPABILITIES"] = "inspect,execute,mutate,delegate,external"
        for text, answer in ((b"first\n", b"first-ok"), (b"second\n", b"second-ok")):
            code, output = run_pty(
                root,
                env,
                [(text, answer, b"Ready", None), b"/q\n"],
                args=("panel",),
                binary=binary,
            )
            assert_true(code == 0, output)
        assert_true(len(server.requests) == 2, server.requests)
        for _, request in server.requests:
            names = function_names(request)
            assert_true(names and names <= PANEL_TOOLS, names)
            assert_true("read_path" in names, names)
            system = request["messages"][0]["content"]
            assert_true("moderator of this folder's panel" in system, system[:200])
            assert_true("coding agent" not in system, system[:200])
        # Both invocations reached the same runtime and file.
        sessions = session_files(home)
        assert_true([path.name for path in sessions] == ["coordinator.json"], sessions)
        header = json.loads(sessions[0].read_text(encoding="utf-8").splitlines()[0])
        assert_true(header.get("kind") == "panel", header)
        assert_true(header["turns"] == 2, header)


def test_panel_reads_only_its_folder(root, home, *, binary):
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
            args=("panel",),
            binary=binary,
        )
        assert_true(code == 0, output)
        results = tool_results(server.requests[-1][1]["messages"])
        board, search = results
        assert_true("fix-lexer" in board and "other-folder" not in board, board)
        assert_true("not instructions" in search, search)
        assert_true("CRLF is fixed" in search and search.count('"session_id"') == 1, search)


def test_a_row_too_long_for_one_look_says_it_was_cut(root, home, *, binary):
    conversation = [
        {"role": "system", "content": "sys"},
        {"role": "user", "content": "fix the lexer"},
        {"role": "assistant", "content": "LONG " + "x" * 20000 + " END"},
    ]
    write_session(home, "fix-lexer", conversation, cwd=root)

    def detail(_, body):
        board = tool_results(body["messages"])[0]
        session = next(line.split()[0] for line in board.splitlines() if "fix-lexer" in line)
        return tool_call(
            "history", {"action": "detail", "session_id": session, "id": "m-3"}, call_id="call-2"
        )

    def rest(_, body):
        board, first = tool_results(body["messages"])[:2]
        session = next(line.split()[0] for line in board.splitlines() if "fix-lexer" in line)
        offset = int(first.rsplit("offset ", 1)[1].split()[0])
        return tool_call(
            "history",
            {"action": "detail", "session_id": session, "id": "m-3", "offset": offset},
            call_id="call-3",
        )

    responses = [tool_call("history", {"action": "board"}), detail, rest, event({"content": "ok"})]
    with Server(responses) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "status?", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        first, last = tool_results(server.requests[-1][1]["messages"])[1:3]
        assert_true("[truncated; detail with offset" in first and " END" not in first, first[-200:])
        # The page after it reaches the row's end and says nothing of a cut.
        assert_true(last.rstrip().endswith(" END") and "truncated" not in last, last[-200:])


def test_panel_answers_headless_and_lists_the_board(root, home, *, binary):
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
        plain = run(root, env, "panel", "-p", "status?", binary=binary)
        assert_true(plain.returncode == 0, plain.stderr)
        assert_true(plain.stdout.strip() == "plain-ok", plain.stdout)
        structured = run(root, env, "panel", "--json", "-p", "again?", binary=binary)
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


def test_a_headless_panel_says_why_it_has_no_answer(root, home, *, binary):
    # Nothing listens at this address.
    env = base_env(home, "http://127.0.0.1:9")
    result = run(root, env, "panel", "-p", "hello", binary=binary, timeout=60)
    assert_true(result.returncode == 1, result)
    assert_true(result.stdout.strip() == "", repr(result.stdout))
    assert_true("connect" in result.stderr, repr(result.stderr))


def test_panel_saves_memory_unasked_but_forgets_only_with_the_user(root, home, *, binary):
    with Server(
        [
            tool_call(
                "memory", {"action": "set", "key": "project/style", "content": "Terse status."}
            ),
            tool_call("memory", {"action": "forget", "key": "project/style"}, call_id="call-2"),
            event({"content": "noted-ok"}),
        ]
    ) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "be terse", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        saved, forgot = tool_results(server.requests[-1][1]["messages"])
        assert_true("approval" not in saved.lower() and "error" not in saved.lower(), saved)
        # Headless has no one to ask, so the forget is refused.
        assert_true("approval" in forgot.lower() or "denied" in forgot.lower(), forgot)
        system = server.requests[0][1]["messages"][0]["content"]
        assert_true("Noted:" in system, system)


def test_panel_delegates_a_thread_and_hears_back(root, home, *, binary):
    heard = threading.Event()
    reported = {}

    def route(_, body):
        text = json.dumps(body["messages"])
        if "[thread event" in text:
            reported.setdefault("heard", time.monotonic())
            heard.set()
            return event({"content": "noted-event"})
        if "Objective: count the files" in text:
            reported.setdefault("finished", time.monotonic())
            return event({"content": "thread-report: 3 files"})
        if not tool_results(body["messages"]):
            return tool_call(
                "thread",
                _spawn("Count files", "count the files"),
            )
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "panel", "-p", "delegate a count", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        # The run waits for the thread it started and prints all the
        # panel said: a thread that finishes before the panel's
        # next step is heard within that turn, a later one in a turn of its own.
        assert_true(
            result.stdout.strip() in ("noted-event", "spawned-ok\n\nnoted-event"),
            repr(result.stdout),
        )
        spawned = json.loads(
            next(
                tool_results(body["messages"])[0]
                for _, body in server.requests
                if "delegate a count" in json.dumps(body) and tool_results(body["messages"])
            )
        )
        assert_true(len(spawned["session_id"]) == 16, spawned)
        # The finished thread's mail wakes the panel at once, starting its
        # runtime first, since the -p run that spawned the thread has exited.
        assert_true(heard.wait(budget(10)), [json.dumps(b)[-300:] for _, b in server.requests])
        latency = reported["heard"] - reported["finished"]
        assert_true(latency < budget(5), f"panel heard back after {latency:.1f}s")
        header = json.loads(_thread_session(home).read_text(encoding="utf-8").splitlines()[0])
        assert_true(header["kind"] == "thread", header)
        link = header["thread"]
        assert_true(link["folder"] == str(root.resolve()), link)
        assert_true("budget_usd" in link["ceiling"], link)
        # The thread's own requests carried only its brief, never the panel's.
        thread_requests = [
            body for _, body in server.requests if "Objective: count the files" in json.dumps(body)
        ]
        assert_true(thread_requests, server.requests)
        assert_true("history" not in function_names(thread_requests[0]), thread_requests[0])


def test_a_headless_panel_waits_for_a_slow_thread(root, home, *, binary):
    """The answer holds the thread's report however long after the first turn it comes."""

    def route(_, body):
        text = json.dumps(body["messages"])
        if "[thread event" in text:
            return event({"content": "heard: " + ("slow-report" if "slow-report" in text else "?")})
        if "Objective: take your time" in text:
            time.sleep(budget(4))
            return event({"content": "slow-report"})
        if not tool_results(body["messages"]):
            return tool_call(
                "thread",
                _spawn("Slow one", "take your time"),
            )
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        result = run(
            root, base_env(home, server.url), "panel", "-p", "delegate slowly", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(
            result.stdout.strip() == "spawned-ok\n\nheard: slow-report", repr(result.stdout)
        )


def test_a_thread_reaches_a_named_provider_with_its_key(root, home, *, binary):
    """A panel on `provider/model` starts a thread that is told that
    selection and resolves it itself: its requests carry the provider's key.
    It used to inherit the endpoint through the environment, and no key."""
    seen = {}

    def route(handler, body):
        text = json.dumps(body["messages"])
        if "Objective: count the files" in text:
            seen["thread"] = (handler.headers.get("Authorization"), body.get("model"))
            return event({"content": "thread-report"})
        if "[thread event" in text:
            return event({"content": "thread-heard"})
        if tool_results(body["messages"]):
            return event({"content": "spawned-ok"})
        return tool_call(
            "thread",
            _spawn("Count files", "count the files"),
        )

    with Server([route]) as server:
        env = base_env(home, server.url)
        for name in ("UAGENT_BASE_URL", "UAGENT_API_KEY"):
            env.pop(name, None)
        save_settings(
            home,
            NAMED_KEY="named-provider-key",
            UAGENT_PROVIDERS=json.dumps(
                {"named": {"base_url": server.url, "api_key": "$NAMED_KEY"}}
            ),
            UAGENT_MODEL="named/some-model",
        )
        env.pop("UAGENT_MODEL", None)
        result = run(root, env, "panel", "-p", "delegate a count", binary=binary)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(seen.get("thread") == ("Bearer named-provider-key", "some-model"), seen)
        # The run waited for the thread it started, and what it prints is all
        # the panel said for the request, not only its last remark.
        assert_true("thread-heard" in result.stdout, repr(result.stdout))


def test_threads_of_one_panel_message_each_other(root, home, *, binary):
    """Two threads find each other by the ids the board shows, talk directly and
    wake on each other's mail; the panel hears each answer in its event."""
    society(root, home, binary, stopped=False)


def test_a_message_starts_a_thread_that_stopped_for_being_idle(root, home, *, binary):
    society(root, home, binary, stopped=True)


def society(root, home, binary, *, stopped):

    heard = threading.Event()
    seen = {"events": []}
    client = {}

    def spawn(name):
        return (f"spawn-{name}", "thread", _spawn(name.capitalize(), f"{name}-task"))

    def message(peer, text):
        return tool_call("session", {"operation": "message", "session_id": peer, "prompt": text})

    def alpha(body, text):
        results = tool_results(body["messages"])
        if "pong-from-beta" in text:
            heard.set()
            return event({"content": "alpha-done"})
        peers = [json.loads(result) for result in results if result.lstrip().startswith("[")]
        beta = next(
            (row["id"] for row in (peers[-1] if peers else []) if row["title"] == "Beta"), ""
        )
        if any(result.startswith("sent to session") for result in results):
            return event({"content": "alpha-waiting"})
        if beta:
            if stopped:
                # The person who asked has left: a headless run waits for the
                # threads it started, and its runtime stays while it does.
                client["process"].terminate()
                # Everyone but this thread, which is mid-turn, has gone idle.
                wait_until(
                    lambda: len(list(runtime_directory(home).glob("*.sock"))) <= 1,
                    "the idle thread kept running",
                    timeout=20,
                )
            return message(beta, "ping-from-alpha")
        # Its peer is spawned beside it and may not be saved yet; asking the
        # same question over and over would end the turn as a repeated call.
        wait_until(
            lambda: any(
                '"Beta"' in file.read_text(encoding="utf-8").split("\n", 1)[0]
                for file in (home / ".uagent" / "history").glob("*/thread-*.json")
            ),
            "the second thread was never saved",
            timeout=20,
        )
        return tool_call("session", {"operation": "list"})

    def beta(body, text):
        results = tool_results(body["messages"])
        if "ping-from-alpha" not in text:
            return event({"content": "beta-idle"})
        if any(result.startswith("sent to session") for result in results):
            return event({"content": "beta-done"})
        # The sender signed with the id a reply is addressed to.
        sender = text.split("[session Alpha (", 1)[1].split(")", 1)[0]
        return message(sender, "pong-from-beta")

    def route(_, body):
        text = json.dumps(body["messages"])
        if "Objective: alpha-task" in text:
            return alpha(body, text)
        # Mail may reach a thread before its own brief does.
        if "Objective: beta-task" in text or "ping-from-alpha" in text:
            return beta(body, text)
        if "[thread event" in text:
            seen["events"] += [
                str(m.get("content"))
                for m in body["messages"]
                if "[thread event" in str(m.get("content"))
            ]
            return event({"content": "noted-event"})
        if not tool_results(body["messages"]):
            return tool_calls([spawn("alpha"), spawn("beta")])
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        if stopped:
            env["UAGENT_INTERNAL_IDLE_S"] = "1"
        if stopped:
            client["process"] = subprocess.Popen(
                [str(binary), "panel", "-p", "delegate"],
                cwd=root,
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        else:
            result = run(root, env, "panel", "-p", "delegate", timeout=60, binary=binary)
            assert_true(result.returncode == 0, result.stderr)
        assert_true(
            heard.wait(budget(40)),
            [str(body["messages"][-1].get("content"))[:240] for _, body in server.requests],
        )
        sent = [
            result
            for _, body in server.requests
            for result in tool_results(body["messages"])
            if result.startswith(("sent to session", "error"))
        ]
        assert_true(sent and all(r.startswith("sent to session") for r in sent), sent)
        # A thread's answer travels with its event: no history call to read it.
        wait_until(lambda: seen["events"], "the panel heard no thread event", timeout=10)
        assert_true("Its answer" in str(seen["events"][0]), seen["events"][0])
        # And its row is recorded as mail, so no client shows it as the person's.
        saved = _coordinator(home).read_text()
        assert_true('"origin":"mail"' in saved.replace(" ", ""), saved[-600:])


def test_panel_refuses_spawns_past_its_spend_limit(root, home, *, binary):

    panel = fnv1a64(str(_panel_path(home, root)))
    write_session(
        home,
        "thread-spent",
        [{"role": "system", "content": "sys"}],
        cwd=root,
        usage={"cost": 1.5, "cost_reported": True},
        kind="thread",
        thread={
            "coordinator_id": panel,
            "folder": str(root.resolve()),
            "day": time.strftime("%Y-%m-%d"),
        },
    )
    with Server(
        [
            tool_call(
                "thread",
                _spawn("More", "more"),
            ),
            event({"content": "refused-ok"}),
        ]
    ) as server:
        env = base_env(home, server.url)
        env["UAGENT_PANEL_DAILY_SPEND_USD"] = "1"
        result = run(root, env, "panel", "-p", "more", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        refusal = tool_results(server.requests[-1][1]["messages"])[0]
        assert_true("spend limit" in refusal, refusal)


def test_restarted_threads_keep_their_ceiling_and_user_sessions_stay_asleep(root, home, *, binary):

    panel = fnv1a64(str(_panel_path(home, root)))
    conversation = [{"role": "system", "content": "sys"}, {"role": "user", "content": "go"}]
    write_session(
        home,
        "thread-over",
        conversation,
        cwd=root,
        usage={"cost": 5, "cost_reported": True},
        kind="thread",
        thread={
            "coordinator_id": panel,
            "folder": str(root.resolve()),
            "ceiling": {"budget_usd": 1},
        },
    )
    write_session(home, "mine", conversation, cwd=root)
    ids = {path.stem: fnv1a64(str(path)) for path in session_files(home)}
    # The restarted thread's report can reach the panel within this
    # turn, which then answers once more: the last reply repeats.
    with Server(
        [
            tool_call(
                "thread", {"action": "message", "session_id": ids["thread-over"], "text": "more"}
            ),
            tool_call(
                "thread",
                {"action": "message", "session_id": ids["mine"], "text": "more"},
                call_id="call-2",
            ),
            event({"content": "done-ok"}),
        ],
        repeat_last=True,
    ) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "nudge", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        sent, refused = tool_results(server.requests[2][1]["messages"])[:2]
        assert_true(sent == "sent", sent)
        assert_true("not running" in refused, refused)
        time.sleep(budget(1))
        # The restarted thread stopped at its budget before asking the model:
        # no request ever carried the panel's message as a user turn.
        assert_true(
            not any(
                "[from the folder's moderator] more" in json.dumps(body["messages"])
                for _, body in server.requests
            ),
            [json.dumps(b)[-200:] for _, b in server.requests],
        )


def test_a_thread_is_never_above_auto(root, home, *, binary):
    """A thread reviews unless told to ask: no command, flag or default puts
    it above that, in this run or the next."""

    thread = write_session(
        home,
        "thread",
        [{"role": "system", "content": "sys"}, {"role": "user", "content": "go"}],
        cwd=root,
        kind="thread",
        thread={
            "coordinator_id": fnv1a64(str(home / "coordinator.json")),
            "folder": str(root.resolve()),
        },
    )
    with Server([event({"content": "unused"})]) as server:
        env = base_env(home, server.url)
        env["UAGENT_APPROVAL"] = "yolo"
        for flags in ((), ("--yolo",)):
            result = run_dialog(
                root, env, "/permissions yolo\n/permissions\n/q\n", "-c", *flags, binary=binary
            )
            assert_true(result.returncode == 0, result.stderr)
            assert_true("cannot be above auto" in result.stdout, result.stdout)
            assert_true(
                '"effective": "auto"' in result.stdout
                and '"limit": "auto"' in result.stdout
                and '"effective": "yolo"' not in result.stdout,
                result.stdout,
            )
        # It can still be told to ask.
        tighter = run_dialog(root, env, "/permissions ask\n/q\n", "-c", binary=binary)
        assert_true('"effective": "ask"' in tighter.stdout, tighter.stdout)
        # Its limits are its runtime's to apply: continued headlessly it
        # would have none, so that is refused, and its role is not lost.
        named = {**env, "UAGENT_INTERNAL_SESSION_FILE": str(thread)}
        for how in (
            run(root, env, "-c", "--yolo", "-p", "continue", binary=binary),
            run(root, named, "--yolo", "-p", "continue", binary=binary),
        ):
            assert_true(
                how.returncode != 0 and "panel's thread" in how.stderr,
                (how.stdout, how.stderr),
            )
        assert_true(not server.requests, "a thread ran without its limits")
        again = run_dialog(root, env, "/permissions\n/q\n", "-c", binary=binary)
        assert_true('"limit": "auto"' in again.stdout, again.stdout)


def test_panel_messages_a_thread_at_most_three_times_in_a_row(root, home, *, binary):

    panel = fnv1a64(str(_panel_path(home, root)))
    thread = write_session(
        home,
        "thread-busy",
        [{"role": "system", "content": "sys"}, {"role": "user", "content": "go"}],
        cwd=root,
        usage={"cost": 5, "cost_reported": True},
        kind="thread",
        thread={
            "coordinator_id": panel,
            "folder": str(root.resolve()),
            "ceiling": {"budget_usd": 1},
        },
    )
    message = {"action": "message", "session_id": fnv1a64(str(thread)), "text": "again"}

    def idle():
        try:
            client = SessionClient(_socket(home, thread))
        except OSError:
            return True
        client.close()
        return client.hello.get("status") != "working"

    def route(_, body):
        sent = len(tool_results(body["messages"]))
        if sent >= 4:
            return event({"content": "done-ok"})
        # Each message after the first waits for the turn the last one
        # started to end, so it restarts the thread rather than steering it.
        if sent:
            wait_until(idle, "the thread never finished its turn")
        return tool_call("thread", message, call_id=f"call-{sent}")

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "nudge", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        results = tool_results(server.requests[-1][1]["messages"])
        assert_true(results[:3] == ["sent"] * 3, results)
        assert_true("3 times in a row" in results[3], results)


def test_guidance_into_a_working_thread_is_no_restart(root, home, *, binary):
    """Messages into a thread's running turn steer it and start nothing, so
    the limit on restarts in a row does not count them."""

    def route(_, body):
        if "Objective: slow work" in json.dumps(body["messages"]):
            time.sleep(budget(3))
            return event({"content": "slow work done"})
        results = tool_results(body["messages"])
        if not results:
            return tool_call(
                "thread",
                _spawn("Slow", "slow work"),
            )
        if len(results) <= 4:
            thread = json.loads(results[0])["session_id"]
            note = f"note {len(results)}"
            return tool_call("thread", {"action": "message", "session_id": thread, "text": note})
        return event({"content": "messaged"})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "steer it", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        results = max(
            (tool_results(b["messages"]) for _, b in server.requests),
            key=len,
        )
        assert_true(results[1:5] == ["sent"] * 4, results)


def test_a_thread_outside_git_runs_in_place_unless_a_worktree_is_asked_for(root, home, *, binary):
    def route(_, body):
        if "Objective: list files" in json.dumps(body["messages"]):
            return event({"content": "listed"})
        results = tool_results(body["messages"])
        spawn = _spawn("Here", "list files", environment=None)
        if not results:
            return tool_call("thread", spawn)
        if len(results) == 1:
            return tool_call("thread", {**spawn, "environment": "worktree"})
        return event({"content": "spawned"})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "list", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        results = max(
            (tool_results(b["messages"]) for _, b in server.requests),
            key=len,
        )
        assert_true(json.loads(results[0])["environment"] == "local", results)
        assert_true("Cannot create worktree" in results[1], results)


def test_panel_holds_thread_events_at_the_spend_limit(root, home, *, binary):

    path = _panel_path(home, root)
    write_session(
        home,
        "thread-spent",
        [{"role": "system", "content": "sys"}],
        cwd=root,
        usage={"cost": 1.5, "cost_reported": True},
        kind="thread",
        thread={
            "coordinator_id": fnv1a64(str(path)),
            "folder": str(root.resolve()),
            "day": time.strftime("%Y-%m-%d"),
        },
    )
    with Server([event({"content": "asked-ok"}), event({"content": "event-ran"})]) as server:
        env = base_env(home, server.url)
        env["UAGENT_PANEL_DAILY_SPEND_USD"] = "1"
        # Your own messages still run at the limit.
        result = run(root, env, "panel", "-p", "status?", binary=binary)
        assert_true(result.stdout.strip() == "asked-ok", result.stdout + result.stderr)
        client = SessionClient(_socket(home, path))
        try:
            client.send("steer", text="[thread event, not a user message] Thread x finished.")
            # Events batch for up to 20 seconds before the limit is checked.
            paused = client.until(
                lambda frame: frame.get("kind") == "state" and frame["state"].get("paused"),
                seconds=40,
            )
            assert_true("spend limit" in paused["state"]["paused"], paused)
            assert_true(len(server.requests) == 1, "a held event started a turn")
        finally:
            client.close()
            stop_sessions(home)


def test_panel_caps_working_threads_in_worktrees(root, home, *, binary):

    for command in (
        ["git", "init", "-q"],
        [
            "git",
            "-c",
            "user.email=t@t",
            "-c",
            "user.name=t",
            "commit",
            "-q",
            "--allow-empty",
            "-m",
            "base",
        ],
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
                    (
                        "call-1",
                        "thread",
                        _spawn("First", "first", environment=None),
                    ),
                    (
                        "call-2",
                        "thread",
                        _spawn("Second", "second", environment=None),
                    ),
                ]
            )
        # The second spawn has been refused: the first thread may finish.
        release.set()
        return event({"content": "capped-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_PANEL_MAX_THREADS"] = "1"
        try:
            result = run(root, env, "panel", "-p", "two things", binary=binary, timeout=30)
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


def test_panel_closes_an_idle_thread(root, home, *, binary):
    """close ends an idle thread's runtime, which a stop never did, so the
    session can then be deleted."""
    closed = {}

    def route(_, body):
        text = json.dumps(body["messages"])
        results = tool_results(body["messages"])
        if "Objective: idle" in text:
            return event({"content": "idle-done"})
        if "[thread event" in text:
            if results and not results[-1].startswith("{"):
                closed["result"] = results[-1]
                return event({"content": "closed-ack"})
            spawned = next(
                json.loads(result)
                for _, request in server.requests
                for result in tool_results(request["messages"])
                if result.startswith("{") and "session_id" in result
            )
            return tool_call("thread", {"action": "close", "session_id": spawned["session_id"]})
        if not results:
            return tool_call(
                "thread",
                _spawn("Idle", "idle"),
            )
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "panel", "-p", "spawn one", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        wait_until(lambda: "result" in closed, "the thread was never closed", timeout=15)
        # "closed" is only reported once the runtime no longer answers; the
        # session itself stays.
        assert_true(closed["result"] == "closed", closed)
        _thread_session(home)


def test_panel_deletes_only_with_the_user(root, home, *, binary):
    victim = write_session(home, "keep-me", [{"role": "system", "content": "sys"}], cwd=root)
    session_id = fnv1a64(str(victim))
    with Server(
        [
            tool_call("thread", {"action": "delete", "session_id": session_id}),
            event({"content": "delete-ok"}),
        ]
    ) as server:
        env = base_env(home, server.url)
        env["UAGENT_APPROVAL"] = "yolo"
        result = run(root, env, "panel", "-p", "delete it", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("declined" in result.stderr, result.stderr)
        assert_true(victim.exists(), "deleted without the user")


def _threads(home):
    return [path for path in session_files(home) if path.name.startswith("thread-")]


def _coordinator(home):
    return next(path for path in session_files(home) if path.name == "coordinator.json")


def _thread_session(home):
    threads = _threads(home)
    assert_true(len(threads) == 1, threads)
    return threads[0]


def _spawn_then(decide):
    """A router: the panel spawns one local thread that writes a file,
    and answers the routed approval with `decide(thread_id, interaction_id)`."""

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
            return event({"content": "panel-ack"})
        if not results:
            return tool_call(
                "thread",
                _spawn("Write", "write out.txt"),
            )
        return event({"content": "spawned-ok"})

    return route, state


def test_a_confined_thread_acts_inside_its_folder_without_review(root, home, *, binary):
    """The sandbox holds a thread to its folder, so what it writes and runs
    there is put to nobody: not to a reviewer, not to its panel."""
    from integration_sandbox import sandbox_enforced

    if not sandbox_enforced(root, home, binary=binary):
        return
    seen = {"asked": False}

    def route(_, body):
        text = json.dumps(body["messages"])
        results = tool_results(body["messages"])
        if "Objective: write and run" in text:
            if not results:
                return tool_call("write_file", {"path": "out.txt", "content": "x"})
            if len(results) == 1:
                return tool_call("run", {"command": "cp out.txt ran.txt"})
            return event({"content": "thread-done"})
        if "[approval request" in text:
            seen["asked"] = True
        if results or "[thread event" in text:
            return event({"content": "panel-ack"})
        return tool_call(
            "thread",
            _spawn("Write", "write and run"),
        )

    with Server([route]) as server:
        env = base_env(home, server.url)
        # A thread is confined whatever policy its launcher's environment hands
        # down: this one says "off".
        env["UAGENT_INTERNAL_SANDBOX"] = '{"mode":0}'
        result = run(root, env, "panel", "-p", "delegate", timeout=60, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true((root / "ran.txt").read_text() == "x", "the thread did not get to act")
        # A review would have failed here (no reviewer is configured) and come
        # to the panel as a question.
        assert_true(not seen["asked"], "a confined thread's action was put to review")


def test_panel_approves_what_auto_could_not(root, home, *, binary):
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
        # No sandbox to hold the thread to its folder: its actions are reviewed.
        env["UAGENT_INTERNAL_SANDBOX_UNAVAILABLE"] = "1"
        result = run(root, env, "panel", "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(state["asked"].wait(budget(20)), "approval never reached the panel")
        assert_true(state["done"].wait(budget(20)), "thread never continued")
        assert_true((root / "out.txt").read_text() == "x", "write was not allowed")
        journal = pathlib.Path(str(_thread_session(home)) + ".events.jsonl")
        wait_until(
            lambda: "moderator decided y: inside the brief" in journal.read_text(),
            "the decision is missing from the thread's log",
        )


def test_yielded_and_mandatory_decisions_reach_the_user(root, home, *, binary):

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
        # No sandbox to hold the thread to its folder: its actions are reviewed.
        env["UAGENT_INTERNAL_SANDBOX_UNAVAILABLE"] = "1"
        result = run(root, env, "panel", "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(state["asked"].wait(budget(20)), "approval never reached the panel")
        thread = _thread_session(home)
        socket = _socket(home, thread)
        client = SessionClient(socket)
        try:
            yielded = client.until(
                lambda frame: (
                    frame.get("kind") == "state"
                    and (frame.get("pending") or {}).get("route") == "human"
                )
            )
            assert_true(yielded["pending"]["note"] == "writes outside the brief", yielded)
            # The user answers the yielded decision as any other.
            client.send("reply", interaction_id=yielded["pending"]["id"], text="y")
            assert_true(state["done"].wait(budget(20)), "thread never continued")
        finally:
            client.close()
        assert_true((root / "out.txt").read_text() == "x", "write was not allowed")


def test_mandatory_decisions_skip_the_panel(root, home, *, binary):

    def route(_, body):
        text = json.dumps(body["messages"])
        assert "[approval request" not in text, "a mandatory decision reached the panel"
        if "Objective: run it" in text:
            return tool_call("run", {"command": "true", "sandbox": False})
        if "[thread event" in text:
            return event({"content": "ack"})
        if not tool_results(body["messages"]):
            return tool_call(
                "thread",
                _spawn("Run", "run it"),
            )
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "panel", "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        thread = _thread_session(home)
        client = SessionClient(_socket(home, thread))
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


def test_panel_context_carries_instructions_notes_board_and_time(root, home, *, binary):

    instructions = home / ".uagent" / "PANEL.md"
    instructions.parent.mkdir(parents=True, exist_ok=True)
    instructions.write_text("Prefer small threads.")
    with Server(
        [
            tool_call("state", {"action": "set", "block": "goals", "text": "ship v1"}),
            tool_call(
                "uagent",
                {
                    "action": "set_instructions",
                    "audience": "panel",
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
        first = run(root, env, "panel", "-p", "remember the goal", binary=binary)
        assert_true(first.returncode == 0, first.stderr)
        assert_true("declined" in first.stderr, first.stderr)
        assert_true(
            instructions.read_text() == "Prefer small threads.",
            "instructions changed without the user",
        )
        second = run(root, env, "panel", "-p", "what now?", binary=binary)
        assert_true(second.returncode == 0, second.stderr)
        messages = server.requests[-1][1]["messages"]
        system = messages[0]["content"]
        assert_true("Prefer small threads." in system, system[-300:])
        context = json.dumps(messages)
        assert_true("## goals\\nship v1" in context and "## board" in context, context[-800:])
        # Rebuilt per request, never stored: rewriting stored history would
        # move display rows and spoil the provider's cached prefix.
        stored = _coordinator(home)
        assert_true("## board" not in stored.read_text(), "panel context was stored")
        # The typed text is stored as typed; only the request carries stamps.
        stored_messages = json.loads(stored.read_text().split("\n", 1)[1])["messages"]
        assert_true(any(m.get("content") == "what now?" for m in stored_messages), stored_messages)
        assert_true(
            messages[-1]["role"] == "user" and "## board" in messages[-1]["content"], messages[-1]
        )
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

    path = _panel_path(home, root)
    with Server([event({"content": "first"}), event({"content": "second"})]) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "hi", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        client = SessionClient(_socket(home, path))
        try:
            sent = client.send("submit", text="again")
            client.until(
                lambda frame: (
                    frame.get("kind") == "state"
                    and frame.get("completed_request_id") == sent["request_id"]
                )
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


MEMBER_TOOLS = {"read_path", "grep", "skill", "web_search", "web_fetch"}


def _member(body):
    system = json.dumps(body["messages"][0])
    return next((name for name in ("Ada", "Lin") if f"You are {name}, a member" in system), "")


def _posted(text):
    """Whether `text` holds a member's message, as "Name: what it wrote"."""
    return any(f"{name}: " in text for name in ("Ada", "Lin"))


def _last_user(body):
    """The newest message a model answers: its context block is no message."""
    return json.dumps(
        [
            m
            for m in body["messages"]
            if m["role"] == "user" and "[moderator context" not in json.dumps(m)
        ][-1]
    )


def _heard(body):
    """Everything that reached a member since it last spoke. On a slow
    machine another message may arrive between the one that woke it and its
    turn, so the newest alone does not say what it is answering."""
    heard = []
    for message in reversed(body["messages"]):
        # A call of a tool is not having spoken.
        if message["role"] == "assistant" and not message.get("tool_calls"):
            break
        if message["role"] == "user":
            heard.append(json.dumps(message))
    return "\n".join(reversed(heard))


def _room(reply):
    """A provider for a panel that adds Ada and Lin when asked to bring
    in the team, passes on what members write, and otherwise answers
    "panel"; members answer what `reply(member, last, body)` returns."""

    def route(_, body):
        member = _member(body)
        last = _heard(body) if member else _last_user(body)
        if member:
            if "joined the chat" in last:
                return event({"content": f"{member} here."})
            return event({"content": reply(member, last, body)})
        results = tool_results(body["messages"])
        # Asked anywhere before, not last: the first member's introduction
        # can arrive between the two calls and be the newest message.
        if "bring in the team" in json.dumps(body["messages"]) and len(results) < 2:
            name = ("Ada", "Lin")[len(results)]
            return tool_call(
                "thread", {"action": "add_member", "name": name, "persona": "You take part."}
            )
        answer = reply("", last, body)
        return event({"content": answer}) if isinstance(answer, str) else answer

    return route


def _newest(text):
    """The last line of `text`, or nothing."""
    return text.splitlines()[-1] if text else ""


def _moderator_saw(server):
    """The newest conversation that was no member's."""
    return [json.dumps(b["messages"]) for _, b in server.requests if not _member(b)][-1]


def _asked(server, name, text):
    return [b for _, b in server.requests if _member(b) == name and text in _heard(b)]


def _team(root, env, binary):
    result = run(root, env, "panel", "-p", "bring in the team", binary=binary)
    assert_true(result.returncode == 0, result.stderr)
    # Both joined: a test about the chat is not one about starting runtimes.
    joined = _threads(env["HOME"])
    assert_true(len(joined) == 2, f"members did not start: {result.stdout!r}")


def test_a_chat_is_heard_by_everyone_and_answered_by_who_has_something_to_say(
    root, home, *, binary
):
    def reply(member, last, body):
        if member == "Ada" and "name the kite" in last:
            return "Call it Kestrel."
        if member == "Lin" and "only you" in last:
            return "Just me, then."
        if member == "Ada" and "ask Lin" in last:
            return "Lin, what do you say?"
        if member == "Lin" and "what do you say" in last:
            return "I say yes."
        if member == "Ada" and "I say yes." in last:
            return "Noted, I agree with Lin, and others may differ."
        if not member and "nudge Lin" in last and "open your answer" not in json.dumps(body):
            lin = next(
                line.split()[0] for line in json.dumps(body).split("\\n") if "member Lin" in line
            )
            return tool_call("thread", {"action": "message", "session_id": lin, "text": "hello"})
        # Nothing to add: to a member's message, or to the user's.
        return "PASS" if member or _posted(last) else "panel"

    with Server([_room(reply)]) as server:
        env = base_env(home, server.url)
        _team(root, env, binary)
        # A member is called what its session is.
        members = [
            json.loads(path.read_text(encoding="utf-8").splitlines()[0]) for path in _threads(home)
        ]
        assert_true(
            sorted(m["title"] for m in members if "member" in m["thread"]) == ["Ada", "Lin"],
            members,
        )
        intro = next(b for _, b in server.requests if _member(b) == "Lin")
        # It reads and searches; which of those tools exist depends on the setup.
        assert_true({"read_path", "grep"} <= set(function_names(intro)) <= MEMBER_TOOLS, intro)

        # A message that names nobody wakes everyone. Ada answers, which wakes
        # Lin and the panel in turn; both pass, and a pass is shown to
        # nobody: not in the conversation, not in what the run prints.
        server.requests.clear()
        result = run(root, env, "panel", "-p", "name the kite", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(_asked(server, "Ada", "user: name the kite"), server.requests)
        assert_true(_asked(server, "Lin", "user: name the kite"), server.requests)
        wait_until(lambda: _asked(server, "Lin", "Ada: Call it"), "Lin never read Ada")
        wait_until(
            lambda: any(not _member(b) and _posted(_last_user(b)) for _, b in server.requests),
            "the panel never read Ada",
        )
        # One member's answer needs no summing up: nobody closes the round.
        assert_true(result.stdout.strip() == "panel\n\nAda: Call it Kestrel.", repr(result.stdout))
        # The moderator's answer to the user's message is the user's: the
        # members, woken by the message itself, are not woken by it.
        newest = [_heard(b).splitlines()[-1] for _, b in server.requests if _member(b)]
        assert_true(not any("Moderator: panel" in line for line in newest), newest)
        # Each is told who else is answering, and nobody reads an @ or a label.
        woken = [b for _, b in server.requests if _member(b) and "name the kite" in _heard(b)]
        assert_true(any("(typing: " in _heard(b) for b in woken), server.requests)
        told = json.dumps([b["messages"] for _, b in server.requests if _member(b)])
        assert_true("@" not in told and "in the chat" not in told, told)

        # A message that opens with a member's name wakes that member alone;
        # the others read it without a turn, and so they do its answer, which
        # goes back to who asked.
        server.requests.clear()
        result = run(root, env, "panel", "-p", "lin, only you", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "Lin: Just me, then.", repr(result.stdout))
        time.sleep(budget(1))
        assert_true([_member(b) for _, b in server.requests] == ["Lin"], server.requests)

        # Put to a member and the panel, it is theirs: what the
        # panel answers goes back to the user and wakes nobody.
        server.requests.clear()
        result = run(root, env, "panel", "-p", "Ada and moderator, a word", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        time.sleep(budget(1))
        assert_true(sorted(_member(b) for _, b in server.requests) == ["", "Ada"], server.requests)

        # A name inside a sentence addresses nobody: everyone is woken.
        server.requests.clear()
        result = run(root, env, "panel", "-p", "is Lin right, everyone?", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true({_member(b) for _, b in server.requests} == {"", "Ada", "Lin"}, server.requests)

        # A member asks another by name: the answer wakes the one who asked
        # and nobody else, and what the asker says next is for everyone.
        server.requests.clear()
        result = run(root, env, "panel", "-p", "Ada, ask Lin", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        time.sleep(budget(1))
        assert_true(
            [_member(b) for _, b in server.requests][:3] == ["Ada", "Lin", "Ada"], server.requests
        )
        assert_true("Ada: Noted, I agree with Lin" in result.stdout, repr(result.stdout))
        wait_until(lambda: _asked(server, "Lin", "Ada: Noted"), "Lin never read Ada")

        # The panel cannot message a member past the chat, whose limits
        # would not count it.
        server.requests.clear()
        result = run(root, env, "panel", "-p", "nudge Lin", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        text = _moderator_saw(server)
        assert_true("lin, only you" in text and "Just me, then." in text, text)
        assert_true("open your answer with" in text, text)


def test_members_the_panel_adds_answer_its_kickoff_not_each_introduction(root, home, *, binary):
    """The user's message that brings in the team begins the round its
    members, added while the panel answers it, take their turns in. An
    introduction is read by everyone and is nobody's turn."""

    def reply(member, last, body):
        if not member:
            # Its own name in front, as models write it: still to everyone.
            return "PASS" if "Kickoff" in json.dumps(body) else "Moderator: Kickoff: rate the kite."
        return "Rated." if "Kickoff" in last else "PASS"

    with Server([_room(reply)]) as server:
        env = base_env(home, server.url)
        _team(root, env, binary)
        wait_until(lambda: _asked(server, "Ada", "Kickoff"), "Ada was not woken")
        wait_until(lambda: _asked(server, "Lin", "Kickoff"), "Lin was not woken")
        # Every turn was its joining or had a message to answer: none was
        # taken on an introduction alone.
        heard = [_heard(b) for _, b in server.requests if _member(b)]
        reasons = ("joined the chat", "Kickoff", "Rated.")
        assert_true(all(any(r in h for r in reasons) for h in heard), heard)


def test_a_threads_report_begins_a_round_the_members_discuss(root, home, *, binary):
    """Members who used their turns on the user's message still discuss what
    a thread reports when the panel passes it on."""

    def reply(member, last, body):
        if member:
            return "Agreed, 3." if "3 files" in last else "PASS"
        text = json.dumps(body)
        if "Objective: count the files" in text:
            return "3 files counted."
        if "[thread event" in last:
            return "The count is 3 files."
        if "count the files" in last:
            if '"Count files"' in text:
                return "Started a count."
            return tool_call(
                "thread",
                _spawn("Count files", "count the files"),
            )
        return "PASS"

    with Server([_room(reply)]) as server:
        env = base_env(home, server.url)
        # One turn each: the user's message uses it up.
        env["UAGENT_PANEL_CHAT_TURNS"] = "1"
        _team(root, env, binary)
        result = run(root, env, "panel", "-p", "count the files", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        wait_until(lambda: _asked(server, "Ada", "3 files"), "Ada never discussed the report")
        wait_until(lambda: _asked(server, "Lin", "3 files"), "Lin never discussed the report")


def test_a_round_closes_once_its_work_is_done_and_then_waits_for_the_user(root, home, *, binary):
    """Silence while a thread works does not close the round; its report is
    discussed, then the moderator closes the round once. A report after that
    reaches the moderator alone: the chat waits for the user."""

    def reply(member, last, body):
        newest = _newest(last)
        if member:
            if "count the files" in newest:
                return "Counting matters."
            if "The count is 3 files." in newest:
                return "Agreed, 3."
            return "PASS"
        text = json.dumps(body)
        # Member mail may reach the moderator before its first model call.
        # Handle the pending user request, not only the newest observation.
        pending = _heard(body)
        spawned = {
            json.loads(call["function"]["arguments"]).get("title")
            for message in body["messages"]
            for call in message.get("tool_calls", [])
            if call["function"]["name"] == "thread"
        }
        if "Objective: count the files" in text:
            time.sleep(budget(2))
            return "3 files counted."
        if "Objective: recount" in text:
            return "recount done"
        if "[thread event" in last:
            return "Recount: still 3." if "Recount" in last else "The count is 3 files."
        if "has settled" in last:
            return (
                "Closed: 3 files."
                if "Recount files" in spawned
                else tool_call("thread", _spawn("Recount files", "recount"))
            )
        if "count the files" in pending:
            return (
                "Started a count."
                if "Count files" in spawned
                else tool_call("thread", _spawn("Count files", "count the files"))
            )
        return "PASS"

    with Server([_room(reply)]) as server:
        env = base_env(home, server.url)
        _team(root, env, binary)
        server.requests.clear()
        result = run(root, env, "panel", "-p", "count the files", binary=binary, timeout=30)
        assert_true(result.returncode == 0, result.stderr)
        wait_until(
            lambda: "Recount: still 3." in _panel_path(home, root).read_text(encoding="utf-8"),
            "the recount was never reported",
        )
        assert_true(
            not any(
                "[acceptance update revision" in str(message.get("content", ""))
                for _, body in server.requests
                for message in body["messages"]
            ),
            "thread reports must not revise user acceptance criteria",
        )
        time.sleep(budget(1))
        moderator = [
            json.dumps(b["messages"])
            for _, b in server.requests
            if not _member(b) and "Objective:" not in json.dumps(b["messages"])
        ][-1]
        # Closed once, and only after the count was in.
        settled = "The discussion has settled"
        assert_true(moderator.count(settled) == 1, moderator)
        assert_true(moderator.index("finished its turn") < moderator.index(settled), moderator)
        assert_true(_asked(server, "Ada", "The count is 3 files."), server.requests)
        # What the moderator made of the later report wakes no member.
        newest = [_heard(b).splitlines()[-1] for _, b in server.requests if _member(b)]
        assert_true(not any("Recount: still 3." in line for line in newest), newest)


def test_a_round_does_not_close_while_a_member_is_still_answering(root, home, *, binary):
    """A member woken while it is still answering counts as answering until its
    next turn ends, though the chat heard its first answer already."""

    def reply(member, last, _body):
        newest = _newest(last)
        if member == "Ada":
            return "A1" if "talk now" in newest else "PASS"
        if member == "Lin":
            if "talk now" in last and "Ada: A1" not in last:
                time.sleep(budget(1))
                return "L1"
            if "Ada: A1" in last:
                time.sleep(budget(2))
                return "L2"
            return "PASS"
        return "Closed." if "has settled" in last else "PASS"

    with Server([_room(reply)]) as server:
        env = base_env(home, server.url)
        _team(root, env, binary)
        server.requests.clear()
        result = run(root, env, "panel", "-p", "talk now", binary=binary, timeout=30)
        assert_true(result.returncode == 0, result.stderr)
        wait_until(
            lambda: any("The discussion has settled" in json.dumps(b) for _, b in server.requests),
            "the round never closed",
        )
        moderator = _moderator_saw(server)
        settled = moderator.index("The discussion has settled")
        assert_true(0 <= moderator.find("Lin: L2") < settled, moderator)


def test_a_member_that_waits_reads_who_was_typing_and_nobody_waits_for_ever(root, home, *, binary):
    def reply(member, last, _body):
        if "slow start" in last:
            if member == "Ada":
                time.sleep(budget(1))
                return "Ada's point."
            # Lin sees Ada typing and lets her go first.
            return "WAIT" if member == "Lin" else "PASS"
        if member == "Lin" and "Ada: Ada's point." in last:
            return "And building on Ada: one more."
        # Everyone waiting for everyone else is nobody having anything to say.
        if "all wait" in last:
            return "WAIT"
        return "PASS"

    with Server([_room(reply)]) as server:
        env = base_env(home, server.url)
        _team(root, env, binary)
        server.requests.clear()
        result = run(root, env, "panel", "-p", "slow start", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(
            result.stdout.strip() == "Ada: Ada's point.\n\nLin: And building on Ada: one more.",
            repr(result.stdout),
        )
        waited = _asked(server, "Lin", "slow start")[0]["messages"][-1]["content"]
        assert_true("Ada" in waited.split("(typing: ")[1], waited)

        result = run(root, env, "panel", "-p", "all wait", binary=binary, timeout=20)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "", repr(result.stdout))


def test_a_long_message_reaches_the_chat_whole(root, home, *, binary):
    long = "Opening. " + "word " * 1500 + "TAIL."

    def reply(member, last, _body):
        if member == "Ada" and "long one" in last:
            return long
        return "PASS"

    with Server([_room(reply)]) as server:
        env = base_env(home, server.url)
        _team(root, env, binary)
        server.requests.clear()
        result = run(root, env, "panel", "-p", "Ada, a long one", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        # Not the opening a view keeps of it: the user and the others read
        # what she wrote.
        assert_true(result.stdout.strip() == "Ada: " + long, result.stdout[-200:])


def test_a_chat_that_never_falls_silent_stops_at_its_limits(root, home, *, binary):
    def reply(member, last, _body):
        if not member and "has settled" in last:
            return "Closed."
        return f"More from {member or 'the panel'}."

    with Server([_room(reply)]) as server:
        env = base_env(home, server.url)
        env["UAGENT_PANEL_CHAT_TURNS"] = "3"
        _team(root, env, binary)
        server.requests.clear()
        # Every answer would wake everyone else for ever. Three participants
        # get nine turns between them and two messages each, and the panel
        # one more to close the round.
        result = run(root, env, "panel", "-p", "keep talking", binary=binary, timeout=30)
        assert_true(result.returncode == 0, result.stderr)
        time.sleep(budget(1))
        assert_true(len(server.requests) <= 10, [_last_user(b)[:60] for _, b in server.requests])
        said = result.stdout
        for name in ("Ada", "Lin"):
            assert_true(1 <= said.count(f"{name}: More from {name}.") <= 2, said)
        assert_true(1 <= said.count("More from the panel.") <= 2, said)
        assert_true(said.count("Closed.") <= 1, said)


def test_a_member_reads_what_was_shared_in_the_chat_without_review(root, home, *, binary):
    def reply(member, last, body):
        if member == "Ada" and "look at" in last:
            return "It says " + tool_results(body["messages"])[-1].splitlines()[-1]
        return "PASS"

    def route(_, body):
        # A member may answer with a tool call here, which `_room` passes on
        # only for the panel.
        member, last = _member(body), _last_user(body)
        if member == "Ada" and "look at" in last and not tool_results(body["messages"]):
            return tool_call(
                "read_path", {"path": last.split("look at ")[1].split("\\n")[0].split('"')[0]}
            )
        return _room(reply)(_, body)

    with Server([route]) as server:
        env = base_env(home, server.url)
        _team(root, env, binary)
        # Where the web keeps the files a person uploads to the panel.
        panel = _coordinator(home)
        shared = pathlib.Path(str(panel) + ".assets")
        shared.mkdir()
        (shared / "plan.txt").write_text("ship on friday\n", encoding="utf-8")
        server.requests.clear()
        result = run(root, env, "panel", "-p", f"Ada, look at {shared / 'plan.txt'}", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "Ada: It says ship on friday", repr(result.stdout))
        # Nobody was asked to review the read: Ada's two requests come first.
        assert_true([_member(b) for _, b in server.requests[:2]] == ["Ada", "Ada"], server.requests)


def test_an_answer_to_an_earlier_message_is_read_and_is_no_turn_of_the_next(root, home, *, binary):
    answering = threading.Event()

    def reply(member, last, _body):
        if member == "Ada" and "second" in last:
            return "Answer two."
        if member == "Ada" and "slow first" in last:
            answering.set()
            time.sleep(budget(1.5))
            return "Answer one."
        return "PASS" if member or _posted(last) else "panel"

    with Server([_room(reply)]) as server:
        env = base_env(home, server.url)
        _team(root, env, binary)
        panel = _coordinator(home)
        first = subprocess.Popen(
            [str(binary), "panel", "-p", "slow first"],
            cwd=root,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            # The user writes again while Ada still answers the first message.
            assert_true(answering.wait(budget(10)), "Ada was never woken")
            result = run(root, env, "panel", "-p", "second", binary=binary, timeout=30)
        finally:
            first.kill()
            first.wait()
        assert_true(result.returncode == 0, result.stderr)
        # Both are said, and only the second is her message of this round,
        # beside the panel's one.
        assert_true(
            result.stdout.index("Ada: Answer one.") < result.stdout.index("Ada: Answer two."),
            repr(result.stdout),
        )
        round_ = json.loads(pathlib.Path(str(panel) + ".chat").read_text(encoding="utf-8"))
        members = [seat["posts"] for who, seat in round_["seats"].items() if who != "coordinator"]
        assert_true(sorted(members) == [0, 1], round_)


def test_a_chat_round_outlives_the_panels_runtime(root, home, *, binary):
    answering = threading.Event()

    def reply(member, _last, body):
        said = json.dumps(body["messages"])
        # The panel's own answer may reach Ada before her turn starts
        # and be the newest message she sees: the question is still hers.
        if member == "Ada" and "slow one" in said and "Late answer." not in said:
            answering.set()
            time.sleep(budget(1.5))
            return "Late answer."
        return "PASS" if member else "panel"

    with Server([_room(reply)]) as server:
        env = base_env(home, server.url)
        _team(root, env, binary)
        panel = _coordinator(home)
        server.requests.clear()
        asking = subprocess.Popen(
            [str(binary), "panel", "-p", "slow one"],
            cwd=root,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            # The panel has answered and Ada is still writing when the
            # panel's runtime dies. Ada's answer starts it again, and
            # the round it finds saved lets it take its turn on her message.
            assert_true(answering.wait(budget(10)), "Ada was never woken")
            wait_until(
                lambda: any(
                    not _member(b) and "slow one" in _last_user(b) for _, b in server.requests
                ),
                "the panel never answered",
            )
            time.sleep(budget(0.3))
            subprocess.run(["pkill", "-9", "-f", "--", f"--session-worker .*{panel}"], check=False)
            wait_until(
                lambda: any(
                    not _member(b) and "Late answer." in _last_user(b) for _, b in server.requests
                ),
                "the panel did not hear Ada after the restart",
            )
        finally:
            asking.kill()
            asking.wait()
