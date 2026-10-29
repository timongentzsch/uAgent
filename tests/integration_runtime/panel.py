import json
import pathlib
import re
import time

from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    fnv1a64,
    function_names,
    run,
    run_pty,
    session_files,
    tool_call,
    tool_calls,
    tool_results,
    user_texts,
    write_session,
)
from session_support import SessionClient, runtime_directory, stop_sessions


def _panel_path(home, root):
    """Where the panel of `root` keeps its session."""
    return home / ".uagent" / "history" / fnv1a64(str(root.resolve())) / "panel.json"


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
    "agent_start",
    "agent_send",
    "agents",
    "decide",
    "ask",
    "todo",
    "browser",
    "artifact",
    "web_search",
    "web_fetch",
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
    # Each saved in its folder's history, as conversations are.
    for name, folder in (("fix-lexer", root), ("other-folder", elsewhere)):
        history = fnv1a64(str(folder.resolve()))
        (home / ".uagent" / "history" / history).mkdir(parents=True, exist_ok=True)
        write_session(home, f"{history}/{name}", conversation, cwd=folder, title=name)
    with Server(
        [
            tool_call("agents", {"action": "read", "part": "search", "query": "CRLF"}),
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
    responses = [tool_call("agent_start", {}) for _ in range(3)]
    responses.append(event({"content": "answered-after-the-loop"}))
    with Server(responses) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "delegate", binary=binary)
        assert_true(result.stdout.strip() == "answered-after-the-loop", result.stdout)
        assert_true(len(server.requests) == 4, len(server.requests))
        told = "`brief` is required; this call carried nothing"
        messages = server.requests[-1][1]["messages"]
        results = tool_results(messages)
        assert_true(len(results) == 3 and all(told in text for text in results), results)
        # The person and the model both learn why the turn stopped.
        stop = "model repeated an equivalent rejected agent_start call 3 times: " + told
        assert_true(result.returncode == 1 and stop in result.stderr, result)
        assert_true(stop in json.dumps(messages, ensure_ascii=False), messages[-1])
        sessions = session_files(home)
        assert_true([path.name for path in sessions] == ["panel.json"], sessions)


def test_a_lead_agents_commands_and_its_childrens_only_read(root, home, *, binary):
    """A lead agent runs commands without asking, in the readonly sandbox: they
    read the workspace and write nowhere in it, and so do its agents'."""
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
                    ("call-3", "agent_start", {"brief": "child"}),
                ]
            )
        return event({"content": "\n---\n".join(results)})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "panel", "-p", "look", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("hello" in result.stdout, result.stdout)
        assert_true("only read" in result.stdout, result.stdout)
        assert_true(not (root / "by-lead.txt").exists(), "the lead agent wrote")
        assert_true(not (root / "by-child.txt").exists(), "its agent wrote")
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


def _agents_reported(home, root, cost):
    """The panel of `root` has run today, and its agents have since reported
    `cost` to its ledger."""
    panel_path = _panel_path(home, root)
    panel_path.parent.mkdir(parents=True, exist_ok=True)
    today = {"day": time.strftime("%Y-%m-%d"), "baseline": 0, "cost": 0}
    pathlib.Path(f"{panel_path}.pinned").write_text(json.dumps({"spend": today}))
    ledger = home / ".uagent" / "activity" / f"usage-{fnv1a64(str(panel_path))}.jsonl"
    ledger.parent.mkdir(parents=True, exist_ok=True)
    ledger.write_text(json.dumps({"usage": {"cost": cost, "cost_reported": True}}) + "\n")


def test_panel_holds_agent_events_at_the_spend_limit(root, home, *, binary):

    path = _panel_path(home, root)
    _agents_reported(home, root, 1.5)
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
                text='[agent x "y" finished (completed)]',
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


def test_panel_context_carries_instructions_todo_board_and_time(root, home, *, binary):

    instructions = home / ".uagent" / "PANEL.md"
    instructions.parent.mkdir(parents=True, exist_ok=True)
    instructions.write_text("Prefer small threads.")
    items = [{"text": "Tag it", "status": "done"}, {"text": "Ship v1", "status": "in_progress"}]
    with Server(
        [
            tool_call("todo", {"items": items}),
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
        stored = _panel_path(home, root)
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
