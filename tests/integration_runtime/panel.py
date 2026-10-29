import json
import os
import pathlib
import re
import signal
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
    user_texts,
    wait_until,
    write_session,
)
from session_support import SessionClient, runtime_directory, stop_sessions


def _spawn(title, objective, *, environment="local"):
    """A thread spawn's arguments; `environment=None` leaves it to the setting."""
    spawn = {"title": title, "objective": objective}
    return spawn | ({"environment": environment} if environment else {})


def _panel_path(home, root):
    """Where the panel of `root` keeps its session."""
    return home / ".uagent" / "history" / fnv1a64(str(root.resolve())) / "panel.json"


def _thread_role(panel_path, root, **fields):
    """A thread's delegation role, as its panel at `panel_path` writes it."""
    return {
        "parent": fnv1a64(str(panel_path)),
        "parent_session": str(panel_path),
        "lifetime": "thread",
        "folder": str(root.resolve()),
        **fields,
    }


def _board(messages):
    """The board a panel's request carries on its tail."""
    return messages[-1]["content"].split("## board\n", 1)[1]


def _socket(home, session):
    """The runtime socket of the saved session at `session`."""
    return runtime_directory(home) / f"{fnv1a64(str(session))}.sock"


PANEL_TOOLS = {
    "read_path",
    "grep",
    "run",
    "memory",
    "skill",
    "uagent",
    "uagent_configure",
    "uagent_instructions",
    "history",
    "thread_spawn",
    "thread",
    "decide",
    "ask",
    "plan",
    "browser",
    "artifact",
    "web_search",
    "web_fetch",
    "subagent",
    "subagent_fanout",
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
            assert_true("this folder's lead agent" in system, system[:200])
            assert_true("coding agent" not in system, system[:200])
        # Both invocations reached the same runtime and file.
        sessions = session_files(home)
        assert_true([path.name for path in sessions] == ["panel.json"], sessions)
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
            tool_call("history", {"action": "search", "query": "CRLF"}),
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
        messages = server.requests[-1][1]["messages"]
        board = _board(messages)
        [search] = tool_results(messages)
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

    def session(messages):
        return next(
            line.split()[0] for line in _board(messages).splitlines() if "fix-lexer" in line
        )

    def detail(_, body):
        return tool_call(
            "history", {"action": "detail", "session_id": session(body["messages"]), "id": "m-3"}
        )

    def rest(_, body):
        first = tool_results(body["messages"])[0]
        offset = int(first.rsplit("offset ", 1)[1].split()[0])
        return tool_call(
            "history",
            {
                "action": "detail",
                "session_id": session(body["messages"]),
                "id": "m-3",
                "offset": offset,
            },
            call_id="call-2",
        )

    with Server([detail, rest, event({"content": "ok"})]) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "status?", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        first, last = tool_results(server.requests[-1][1]["messages"])
        assert_true("[truncated; detail with offset" in first and " END" not in first, first[-200:])
        # The page after it reaches the row's end and says nothing of a cut.
        assert_true(last.rstrip().endswith(" END") and "truncated" not in last, last[-200:])


def test_panel_answers_headless(root, home, *, binary):
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


def test_a_spawn_without_its_brief_is_turned_away_and_stops_the_turn(root, home, *, binary):
    """A spawn missing its fields never reaches the tool: each time the model
    is told what spawn needs and what its call carried, and the third such
    round ends the turn, which then answers from what it has."""
    responses = [tool_call("thread_spawn", {}) for _ in range(3)]
    responses.append(event({"content": "answered-after-the-loop"}))
    with Server(responses) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "delegate", binary=binary)
        assert_true(result.stdout.strip() == "answered-after-the-loop", result.stdout)
        assert_true(len(server.requests) == 4, len(server.requests))
        told = "`title` and `objective` are required; this call carried nothing"
        messages = server.requests[-1][1]["messages"]
        results = tool_results(messages)
        assert_true(len(results) == 3 and all(told in text for text in results), results)
        # The person and the model both learn why the turn stopped.
        stop = "model repeated an equivalent rejected thread_spawn call 3 times: " + told
        assert_true(result.returncode == 1 and stop in result.stderr, result)
        assert_true(stop in json.dumps(messages, ensure_ascii=False), messages[-1])
        sessions = session_files(home)
        assert_true([path.name for path in sessions] == ["panel.json"], sessions)


def test_a_panel_fans_out_read_only_subagents(root, home, *, binary):
    """The lead agent asks its questions of subagents: one batch, answers in
    the schema it gave, no tool that edits."""
    schema = {"type": "object", "required": ["finding"], "properties": {"finding": {}}}
    offered = []

    def route(_, body):
        messages = body["messages"]
        if any(text.startswith("look at") for text in user_texts(messages)):
            offered.append(function_names(body))
            return event({"content": '{"finding": "fine"}'})
        if not tool_results(messages):
            return tool_calls(
                [
                    ("call-1", "subagent", {"prompt": "look at a", "schema": schema}),
                    ("call-2", "subagent", {"prompt": "look at b", "schema": schema}),
                ]
            )
        return event({"content": "surveyed"})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "survey", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(len(offered) == 2, offered)
        for names in offered:
            assert_true(names and not names & {"write_file", "edit_file"}, names)
        # The lead agent is told so, and does not brief a child to run one.
        [subagent] = [
            t for t in server.requests[0][1]["tools"] if t["function"]["name"] == "subagent"
        ]
        assert_true("their commands only read" in subagent["function"]["description"], subagent)
        last = json.dumps(server.requests[-1][1]["messages"], ensure_ascii=False)
        assert_true(last.count('{\\"finding\\": \\"fine\\"}') == 2, last[-1500:])


def test_a_lead_agents_commands_and_its_childrens_only_read(root, home, *, binary):
    """A lead agent runs commands without asking, in the readonly sandbox: they
    read the workspace and write nowhere in it, and its subagents' do the same."""
    (root / "notes.txt").write_text("hello\n")

    def route(_, body):
        messages = body["messages"]
        results = tool_results(messages)
        if any(t.startswith("child") for t in user_texts(messages)):
            if results:
                return event({"content": "child: " + results[0][:200]})
            return tool_call("run", {"command": "touch by-child.txt"})
        if not results:
            return tool_calls(
                [
                    ("call-1", "run", {"command": "cat notes.txt"}),
                    ("call-2", "run", {"command": "touch by-lead.txt"}),
                    ("call-3", "subagent", {"prompt": "child"}),
                ]
            )
        return event({"content": "\n---\n".join(results)})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "look", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("hello" in result.stdout, result.stdout)
        assert_true("only read" in result.stdout, result.stdout)
        assert_true(not (root / "by-lead.txt").exists(), "the lead agent wrote")
        assert_true(not (root / "by-child.txt").exists(), "its subagent wrote")
        [run_tool] = [t for t in server.requests[0][1]["tools"] if t["function"]["name"] == "run"]
        assert_true("reach no network" in run_tool["function"]["description"], run_tool)


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
        assert_true("approv" in forgot.lower(), forgot)
        system = server.requests[0][1]["messages"][0]["content"]
        assert_true("Noted:" in system, system)


def test_panel_delegates_a_thread_and_hears_back(root, home, *, binary):
    heard = threading.Event()
    reported = {}

    def route(_, body):
        text = json.dumps(body["messages"])
        if "[background agent event" in text:
            reported.setdefault("heard", time.monotonic())
            heard.set()
            return event({"content": "noted-event"})
        if "Objective: count the files" in text:
            reported.setdefault("finished", time.monotonic())
            return event({"content": "thread-report: 3 files"}, usage={"cost": 0.25})
        if not tool_results(body["messages"]):
            return tool_call(
                "thread_spawn",
                _spawn("Count files", "count the files"),
            )
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "panel", "-p", "delegate a count", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        # The run waits for the thread it started and prints what the panel
        # said last, not its "spawned" from before the report.
        assert_true(result.stdout.strip() == "noted-event", repr(result.stdout))
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
        # What the thread spent counts in its panel's session too.
        wait_until(
            lambda: json.loads(_panel_session(home).read_text().splitlines()[0])["cost"] == 0.25,
            "the panel did not count its thread's spend",
        )
        header = json.loads(_thread_session(home).read_text(encoding="utf-8").splitlines()[0])
        assert_true(header["kind"] == "thread", header)
        # One delegation role, as a subagent's, that outlives the panel's turn.
        link = header["delegation"]
        assert_true(link["lifetime"] == "thread" and "parent_session" in link, link)
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
        if "[background agent event" in text:
            return event({"content": "heard: " + ("slow-report" if "slow-report" in text else "?")})
        if "Objective: take your time" in text:
            time.sleep(budget(4))
            return event({"content": "slow-report"})
        if not tool_results(body["messages"]):
            return tool_call(
                "thread_spawn",
                _spawn("Slow one", "take your time"),
            )
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        result = run(
            root, base_env(home, server.url), "panel", "-p", "delegate slowly", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "heard: slow-report", repr(result.stdout))


def test_a_finished_child_does_not_restart_the_idle_period(root, home, *, binary):
    """A child's exit (SIGCHLD) interrupts the runtime's wait but is no input:
    a panel whose thread ended still lets go one idle period after its turn."""
    with Server([event({"content": "done"})]) as server:
        env = base_env(home, server.url)
        env["UAGENT_INTERNAL_IDLE_S"] = "2"
        result = run(root, env, "panel", "-p", "hello", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        [path] = runtime_directory(home).glob("*.sock")
        client = SessionClient(path)
        pid = client.hello["pid"]
        client.close()
        deadline = time.monotonic() + budget(10)
        while path.exists() and time.monotonic() < deadline:
            try:
                os.kill(pid, signal.SIGCHLD)
            except ProcessLookupError:
                break
            time.sleep(0.2)
        assert_true(not path.exists(), "the runtime outlived its idle period")


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
        if "[background agent event" in text:
            return event({"content": "thread-heard"})
        if tool_results(body["messages"]):
            return event({"content": "spawned-ok"})
        return tool_call(
            "thread_spawn",
            _spawn("Count files", "count the files"),
        )

    with Server([route]) as server:
        env = base_env(home, server.url)
        for name in ("UAGENT_BASE_URL", "UAGENT_API_KEY"):
            env.pop(name, None)
        save_settings(
            home,
            binary=binary,
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
        return (f"spawn-{name}", "thread_spawn", _spawn(name.capitalize(), f"{name}-task"))

    def message(peer, text):
        return tool_call("session_message", {"session_id": peer, "prompt": text})

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
        return tool_call("session", {})

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
        if "[background agent event" in text:
            seen["events"] += [
                str(m.get("content"))
                for m in body["messages"]
                if "[background agent event" in str(m.get("content"))
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
        saved = _panel_session(home).read_text()
        assert_true('"origin":"mail"' in saved.replace(" ", ""), saved[-600:])


def _threads_reported(home, root, cost):
    """The panel of `root` has run today, and its threads have since reported
    `cost` to its ledger."""
    panel_path = _panel_path(home, root)
    panel_path.parent.mkdir(parents=True, exist_ok=True)
    today = {"day": time.strftime("%Y-%m-%d"), "baseline": 0, "cost": 0}
    pathlib.Path(f"{panel_path}.pinned").write_text(json.dumps({"spend": today}))
    ledger = home / ".uagent" / "activity" / f"usage-{fnv1a64(str(panel_path))}.jsonl"
    ledger.parent.mkdir(parents=True, exist_ok=True)
    ledger.write_text(json.dumps({"usage": {"cost": cost, "cost_reported": True}}) + "\n")


def test_panel_refuses_spawns_past_its_spend_limit(root, home, *, binary):
    _threads_reported(home, root, 1.5)
    with Server(
        [tool_call("thread_spawn", _spawn("More", "more")), event({"content": "refused-ok"})]
    ) as server:
        env = base_env(home, server.url)
        env["UAGENT_PANEL_DAILY_SPEND_USD"] = "1"
        result = run(root, env, "panel", "-p", "more", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        refusal = tool_results(server.requests[-1][1]["messages"])[0]
        assert_true("spend limit" in refusal, refusal)


def test_restarted_threads_keep_their_ceiling_and_user_sessions_stay_asleep(root, home, *, binary):

    conversation = [{"role": "system", "content": "sys"}, {"role": "user", "content": "go"}]
    write_session(
        home,
        "thread-over",
        conversation,
        cwd=root,
        usage={"cost": 5, "cost_reported": True},
        kind="thread",
        delegation=_thread_role(_panel_path(home, root), root, ceiling={"budget_usd": 1}),
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
                "[from the folder's lead agent] more" in json.dumps(body["messages"])
                for _, body in server.requests
            ),
            [json.dumps(b)[-200:] for _, b in server.requests],
        )


def test_a_thread_may_be_put_in_yolo(root, home, *, binary):
    """A thread reviews (Auto) unless a mode is chosen for its own
    conversation: the environment's yolo does not reach it, /permissions yolo
    does, and its commands then run at the relaxed level."""
    from integration_sandbox import sandbox_enforced

    thread = write_session(
        home,
        "thread",
        [{"role": "system", "content": "sys"}, {"role": "user", "content": "go"}],
        cwd=root,
        kind="thread",
        delegation=_thread_role(home / "panel.json", root),
    )
    sibling = home / "Projects" / "other" / "notes.txt"
    sibling.parent.mkdir(parents=True)
    sibling.write_text("marker-notes\n")
    outputs = []

    def route(_, body):
        messages = body["messages"]
        last = max(i for i, m in enumerate(messages) if m.get("role") == "user")
        results = tool_results(messages[last:])
        if results:
            outputs.append(results[-1])
            return event({"content": "read-ok"})
        return tool_call("run", {"command": f"cat {sibling}; true"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_APPROVAL"] = "yolo"
        result = run_dialog(
            root,
            env,
            "/permissions\nread it\n/permissions yolo\nread it\n/q\n",
            "-c",
            timeout=30,
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        effective = re.findall(r'"effective": "(\w+)"', result.stdout)
        assert_true(effective == ["auto", "yolo"], result.stdout)
        assert_true(len(outputs) == 2, (outputs, result.stdout))
        if sandbox_enforced(root, home, binary=binary):
            assert_true("marker-notes" not in outputs[0], f"auto read a closed folder: {outputs}")
            assert_true("marker-notes" in outputs[1], f"yolo kept it closed: {outputs}")
        # Its limits are its runtime's to apply: continued headlessly it
        # would have none, so that is refused, and its role is not lost.
        named = {**env, "UAGENT_INTERNAL_SESSION_FILE": str(thread)}
        requests = len(server.requests)
        for how in (
            run(root, env, "-c", "--yolo", "-p", "continue", binary=binary),
            run(root, named, "--yolo", "-p", "continue", binary=binary),
        ):
            assert_true(
                how.returncode != 0 and "panel's thread" in how.stderr,
                (how.stdout, how.stderr),
            )
        assert_true(len(server.requests) == requests, "a thread ran without its limits")


def test_a_thread_asks_the_person_in_a_turn_they_started(root, home, *, binary):
    """A decision in a turn the person started, or wrote into, goes to the
    person, and the lead agent never sees it; in a turn the lead agent
    started, it goes to the lead agent first."""
    state = {
        "asked": [],
        "ready": threading.Event(),
        "holding": threading.Event(),
        "release": threading.Event(),
    }

    def route(_, body):
        messages = body["messages"]
        text = json.dumps(messages)
        if "Objective: wait for the user" in text:
            last = max(i for i, m in enumerate(messages) if m.get("role") == "user")
            prompt = str(messages[last].get("content", ""))
            names = ("mine", "theirs", "later")
            name = next((n for n in names if f"write {n}" in prompt), None)
            if name and not tool_results(messages[last:]):
                if name == "later":
                    # Held until the person has written into the turn.
                    state["holding"].set()
                    state["release"].wait(budget(20))
                return tool_call("write_file", {"path": f"{name}.txt", "content": "x"})
            state["ready"].set()
            return event({"content": "thread-done"})
        if "[approval request" in text and "sent " not in json.dumps(tool_results(messages)):
            match = re.search(r"Background agent ([0-9a-f]{16}) .*?interaction ([^)]+)\)", text)
            state["asked"].append(match.group(2))
            return tool_call(
                "decide",
                {
                    "session_id": match.group(1),
                    "interaction_id": match.group(2),
                    "decision": "allow_once",
                    "reason": "inside the brief",
                },
            )
        if "[approval request" in text or "[background agent event" in text:
            return event({"content": "panel-ack"})
        if not tool_results(messages):
            return tool_call("thread_spawn", _spawn("Wait", "wait for the user"))
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        # No sandbox to hold the thread to its folder: its writes are reviewed.
        env["UAGENT_INTERNAL_SANDBOX_UNAVAILABLE"] = "1"
        result = run(root, env, "panel", "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(state["ready"].wait(budget(20)), "the thread never ran its brief")
        client = SessionClient(_socket(home, _thread_session(home)))
        try:

            def idle(frame):
                return (
                    frame.get("kind") == "state"
                    and frame.get("checkpoint")
                    and not frame.get("busy")
                )

            def pending(frame):
                return frame.get("kind") == "state" and frame.get("pending")

            client.until(idle)
            # The person's own message: the decision is theirs.
            client.send("submit", text="write mine")
            mine = client.until(pending)["pending"]
            assert_true(mine.get("route") != "panel", mine)
            client.send("reply", interaction_id=mine["id"], text="y")
            client.until(idle)
            assert_true((root / "mine.txt").exists(), "the person's approval was lost")
            assert_true(mine["id"] not in state["asked"], "the lead agent saw it")
            # The lead agent's message: the decision is its first.
            client.send("steer", origin="panel", text="write theirs")
            theirs = client.until(pending)["pending"]
            assert_true(theirs.get("route") == "panel", theirs)
            wait_until(lambda: (root / "theirs.txt").exists(), "the lead agent never decided")
            assert_true(state["asked"] == [theirs["id"]], state["asked"])
            client.until(idle)
            # The lead agent's turn, once the person writes into it: what it
            # still asks is theirs.
            client.send("steer", origin="panel", text="write later")
            assert_true(state["holding"].wait(budget(20)), "the lead agent's turn never ran")
            joined = client.send("steer", text="go on")
            client.until(
                lambda frame: (
                    frame.get("kind") == "outcome"
                    and frame.get("request_id") == joined["request_id"]
                )
            )
            state["release"].set()
            later = client.until(pending)["pending"]
            assert_true(later.get("route") != "panel", later)
            client.send("reply", interaction_id=later["id"], text="y")
            wait_until(lambda: (root / "later.txt").exists(), "the person's approval was lost")
            assert_true(state["asked"] == [theirs["id"]], state["asked"])
        finally:
            client.close()


def test_panel_messages_a_thread_at_most_three_times_in_a_row(root, home, *, binary):

    thread = write_session(
        home,
        "thread-busy",
        [{"role": "system", "content": "sys"}, {"role": "user", "content": "go"}],
        cwd=root,
        usage={"cost": 5, "cost_reported": True},
        kind="thread",
        delegation=_thread_role(_panel_path(home, root), root, ceiling={"budget_usd": 1}),
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
                "thread_spawn",
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
            return tool_call("thread_spawn", spawn)
        if len(results) == 1:
            return tool_call("thread_spawn", {**spawn, "environment": "worktree"})
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
    _threads_reported(home, root, 1.5)
    with Server([event({"content": "asked-ok"}), event({"content": "event-ran"})]) as server:
        env = base_env(home, server.url)
        env["UAGENT_PANEL_DAILY_SPEND_USD"] = "1"
        # Your own messages still run at the limit.
        result = run(root, env, "panel", "-p", "status?", binary=binary)
        assert_true(result.stdout.strip() == "asked-ok", result.stdout + result.stderr)
        client = SessionClient(_socket(home, path))
        try:
            client.send(
                "steer",
                text="[background agent event, not a user message] Background agent x finished.",
            )
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
        if "[background agent event" in text:
            return event({"content": "noted"})
        if not tool_results(body["messages"]):
            return tool_calls(
                [
                    (
                        "call-1",
                        "thread_spawn",
                        _spawn("First", "first", environment=None),
                    ),
                    (
                        "call-2",
                        "thread_spawn",
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
        if "[background agent event" in text:
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
                "thread_spawn",
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


def _panel_session(home):
    return next(path for path in session_files(home) if path.name == "panel.json")


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
            match = re.search(r"Background agent ([0-9a-f]{16}) .*?interaction ([^)]+)\)", text)
            state["asked"].set()
            return tool_call("decide", decide(match.group(1), match.group(2)))
        if "[approval request" in text or "[background agent event" in text:
            return event({"content": "panel-ack"})
        if not results:
            return tool_call(
                "thread_spawn",
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
        if results or "[background agent event" in text:
            return event({"content": "panel-ack"})
        return tool_call(
            "thread_spawn",
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
            lambda: "lead agent decided y: inside the brief" in journal.read_text(),
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
        if "[background agent event" in text:
            return event({"content": "ack"})
        if not tool_results(body["messages"]):
            return tool_call(
                "thread_spawn",
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


def test_panel_context_carries_instructions_plan_board_and_time(root, home, *, binary):

    instructions = home / ".uagent" / "PANEL.md"
    instructions.parent.mkdir(parents=True, exist_ok=True)
    instructions.write_text("Prefer small threads.")
    items = [{"text": "Tag it", "status": "done"}, {"text": "Ship v1", "status": "in_progress"}]
    with Server(
        [
            tool_call("plan", {"items": items}),
            tool_call(
                "uagent_instructions",
                {
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
        context = messages[-1]["content"]
        assert_true("## board" in context, context)
        assert_true("- [done] Tag it\n- [in_progress] Ship v1\n" in context, context)
        # Rebuilt per request, never stored: rewriting stored history would
        # move display rows and spoil the provider's cached prefix.
        stored = _panel_session(home)
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


def test_headless_panel_says_what_was_not_approved(root, home, *, binary):
    """`panel -p` has nobody to answer an approval and declines it. The model
    reads that it was not approved, and why, never that a user denied it."""
    sibling = home / "Projects" / "other" / "notes.txt"
    sibling.parent.mkdir(parents=True)
    sibling.write_text("marker-notes\n")
    responses = [tool_call("read_path", {"path": str(sibling)}), event({"content": "ok"})]
    with Server(responses) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "read it", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        refused = tool_results(server.requests[-1][1]["messages"])[-1]
    assert_true("not approved" in refused and "sandbox.read" in refused, refused)
    assert_true("user denied" not in refused and "marker-notes" not in refused, refused)
