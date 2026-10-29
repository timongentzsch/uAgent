"""Agents: conversations another conversation started, each in a runtime of
its own, reached through agent_start, agent_send, agents and decide."""

import json
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
    tool_call,
    tool_calls,
    tool_results,
    wait_until,
)
from session_support import SessionClient, runtime_directory

ASKS = re.compile(r"\[agent ([0-9a-f]{16}) .*? \(interaction ([^)]+)\)\]")


def agent_files(home):
    """Every agent saved, wherever it works."""
    return sorted((home / ".uagent" / "history").glob("*/agent-*.json"))


def header(path):
    return json.loads(path.read_text(encoding="utf-8").splitlines()[0])


def brief_of(body):
    """The brief an agent's request starts from, or empty for any other."""
    for message in body["messages"]:
        text = str(message.get("content", ""))
        if message.get("role") == "user" and text.startswith("BRIEF "):
            return text
    return ""


def results_of(body):
    return tool_results(body["messages"])


def text_of(body):
    return json.dumps(body["messages"])


def socket_of(home, path):
    return runtime_directory(home) / f"{fnv1a64(str(path))}.sock"


def client_of(home, path):
    """The person's client of the agent at `path`, once its runtime runs."""
    wait_until(socket_of(home, path).exists, "the agent's runtime never started")
    return SessionClient(socket_of(home, path))


def pending(frame):
    return frame.get("kind") == "state" and frame.get("pending")


def to_person(frame):
    return pending(frame) and frame["pending"].get("route") == "human"


def approval_mode(body):
    return re.findall(r"; approval (\w+)", text_of(body))[-1]


def popen(binary, root, env, *args):
    return subprocess.Popen(
        [str(binary), *args],
        cwd=root,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def test_a_waited_agent_answers_inline_and_keeps_its_conversation(root, home, *, binary):
    """wait returns the agent's answer as the call's result; the agent is a
    saved conversation beside its parent that agent_send starts again."""

    def route(_, body):
        if brief_of(body):
            once_more = "once more" in text_of(body)
            return event({"content": "again: ok" if once_more else "agent says 42"})
        results = results_of(body)
        if not results:
            return tool_call("agent_start", {"brief": "BRIEF answer"})
        if len(results) == 1:
            rows = json.loads(results[0])
            return tool_call("agent_send", {"id": rows[0]["id"], "text": "once more"})
        return event({"content": "parent done"})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "-p", "ask an agent", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "parent done", result.stdout)
        started = next(results_of(b)[0] for _, b in server.requests if len(results_of(b)) == 1)
        (row,) = json.loads(started)
        assert_true(row["state"] == "completed" and "agent says 42" in row["answer"], row)
        (agent,) = agent_files(home)
        role = header(agent)["agent"]
        assert_true(role["edits"] == "none" and role["depth"] == 1, role)
        # Saved beside its parent, which it names by the parent's id.
        assert_true(row["id"] == fnv1a64(str(agent)), (row, agent))
        parents = {fnv1a64(str(p)) for p in agent.parent.glob("*.json")}
        assert_true(role["parent"] in parents, role)
        # agent_send started a turn on the saved agent; its report woke the
        # headless parent, which waited for it before it answered.
        assert_true(any("again: ok" in text_of(b) for _, b in server.requests), "no 2nd turn")
        assert_true(header(agent)["turns"] == 2, header(agent))


def test_an_agent_not_waited_for_reports_to_its_parent_and_its_spend_counts(
    root, home, *, binary
):
    def route(_, body):
        if brief_of(body):
            return event({"content": "counted 3"}, usage={"cost": 0.05})
        text = text_of(body)
        if '" finished (completed)]' in text:
            return event({"content": "heard: " + ("yes" if "counted 3" in text else "?")})
        if not results_of(body):
            return tool_call("agent_start", {"brief": "BRIEF count", "wait": False})
        return event({"content": "started one"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_SESSION_BUDGET"] = "1"
        result = run(root, env, "-p", "count", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        # The headless run waited for the report it was owed.
        assert_true(result.stdout.strip() == "heard: yes", result.stdout)
        (agent,) = agent_files(home)
        # Its ceiling is the parent's budget shared among the free slots.
        assert_true(header(agent)["agent"]["budget"] == 1 / 8, header(agent))
        parent = next(p for p in agent.parent.glob("*.json") if p != agent)
        assert_true(header(parent)["cost"] >= 0.05, header(parent))
    # With too little of its budget left, a conversation starts none.
    with Server([tool_call("agent_start", {"brief": "BRIEF x"}), event({"content": "ok"})]) as s:
        env = base_env(home, s.url)
        env["UAGENT_SESSION_BUDGET"] = "0.001"
        result = run(root, env, "-p", "too poor", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        refused = results_of(s.requests[-1][1])[0]
        assert_true("too little to start agents" in refused, refused)


def test_items_start_one_agent_each_within_agents_max(root, home, *, binary):
    lock = threading.Lock()
    state = {"running": 0, "most": 0}

    def route(_, body):
        brief = brief_of(body)
        if brief:
            with lock:
                state["running"] += 1
                state["most"] = max(state["most"], state["running"])
            time.sleep(budget(0.5))
            with lock:
                state["running"] -= 1
            return event({"content": "did " + brief.rsplit("Item: ", 1)[1]})
        if not results_of(body):
            return tool_call("agent_start", {"brief": "BRIEF part", "items": ["a", "b", "c"]})
        return event({"content": "all in"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_AGENTS_MAX"] = "2"
        result = run(root, env, "-p", "fan out", timeout=30, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        rows = json.loads(results_of(server.requests[-1][1])[0])
        assert_true([row["item"] for row in rows] == ["a", "b", "c"], rows)
        for row in rows:
            assert_true(row["state"] == "completed", rows)
            assert_true("did " + row["item"] in row["answer"], rows)
        assert_true(state["most"] == 2, state)
        assert_true(len(agent_files(home)) == 3, agent_files(home))


def ask_then(decision, *, path="out.txt"):
    """A parent whose agent wants to write a file and asks it first; the
    parent answers with `decision` for the agent and interaction it names."""
    state = {"asked": threading.Event(), "done": threading.Event()}

    def route(_, body):
        if brief_of(body):
            if not results_of(body):
                return tool_call("write_file", {"path": path, "content": "x"})
            state["done"].set()
            return event({"content": "agent wrote"})
        match = ASKS.search(text_of(body))
        if match and decision and "sent " not in json.dumps(results_of(body)):
            state["asked"].set()
            return tool_call("decide", decision(match.group(1), match.group(2)))
        if not results_of(body) and not match:
            return tool_call("agent_start", {"brief": "BRIEF write it", "edits": "folder"})
        return event({"content": "parent ack"})

    return route, state


def test_an_agent_asks_its_parent_which_decides(root, home, *, binary):
    route, state = ask_then(
        lambda agent, interaction: {
            "id": agent,
            "interaction_id": interaction,
            "decision": "allow_once",
            "reason": "inside the brief",
        }
    )
    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        # The wait returned the request rather than hanging on it.
        waited = json.loads(
            next(results_of(b)[0] for _, b in server.requests if ASKS.search(text_of(b)))
        )
        assert_true(waited[0]["state"] == "needs decision", waited)
        assert_true(state["done"].wait(budget(20)), "the agent never continued")
        assert_true((root / "out.txt").read_text() == "x", "the write was not allowed")
        (agent,) = agent_files(home)
        journal = agent.with_name(agent.name + ".events.jsonl")
        wait_until(
            lambda: "parent agent decided y: inside the brief" in journal.read_text(),
            "the decision is missing from the agent's log",
        )


def test_a_decision_its_parent_yields_or_leaves_reaches_the_person(root, home, *, binary):
    route, state = ask_then(
        lambda agent, interaction: {
            "id": agent,
            "interaction_id": interaction,
            "decision": "yield",
            "reason": "writes outside the brief",
        }
    )
    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(state["asked"].wait(budget(20)), "the request never reached the parent")
        client = client_of(home, agent_files(home)[0])
        try:
            yielded = client.until(to_person)["pending"]
            assert_true(yielded["note"] == "writes outside the brief", yielded)
            client.send("reply", interaction_id=yielded["id"], text="y")
            assert_true(state["done"].wait(budget(20)), "the agent never continued")
        finally:
            client.close()
    # A parent that does not decide in time leaves it to the person too.
    route, state = ask_then(None, path="late.txt")
    for path in agent_files(home):
        path.unlink()
    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_INTERNAL_PARENT_DECISION_S"] = "1"
        result = run(root, env, "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        client = client_of(home, agent_files(home)[0])
        try:
            late = client.until(to_person)["pending"]
            assert_true(late["note"] == "The parent agent did not decide in time.", late)
            client.send("reply", interaction_id=late["id"], text="n")
        finally:
            client.close()


def test_a_mandatory_decision_skips_the_parent(root, home, *, binary):
    def route(_, body):
        assert not ASKS.search(text_of(body)), "a mandatory decision reached the parent"
        if brief_of(body):
            return tool_call("run", {"command": "true", "sandbox": False})
        if not results_of(body):
            return tool_call("agent_start", {"brief": "BRIEF run it", "wait": False})
        return event({"content": "started"})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        client = client_of(home, agent_files(home)[0])
        try:
            waiting = client.until(pending)["pending"]
            assert_true(waiting["approval"]["mandatory_human"], waiting)
            assert_true("route" not in waiting, waiting)
            client.send("reply", interaction_id=waiting["id"], text="n")
        finally:
            client.close()


def test_a_person_writing_into_an_agents_turn_decides_it(root, home, *, binary):
    """Once the person writes into a turn the parent started, what that turn
    still asks is theirs, and the parent never sees it."""
    state = {"holding": threading.Event(), "release": threading.Event()}

    def route(_, body):
        assert not ASKS.search(text_of(body)), "the parent saw the person's decision"
        if brief_of(body):
            if not results_of(body):
                state["holding"].set()
                state["release"].wait(budget(20))
                return tool_call("write_file", {"path": "mine.txt", "content": "x"})
            return event({"content": "agent wrote"})
        if not results_of(body):
            return tool_call(
                "agent_start", {"brief": "BRIEF write it", "edits": "folder", "wait": False}
            )
        return event({"content": "started"})

    with Server([route]) as server:
        process = popen(binary, root, base_env(home, server.url), "-p", "delegate")
        try:
            assert_true(state["holding"].wait(budget(20)), "the agent never ran its brief")
            client = client_of(home, agent_files(home)[0])
            try:
                joined = client.send("steer", text="go on")
                client.until(
                    lambda f: (
                        f.get("kind") == "outcome" and f.get("request_id") == joined["request_id"]
                    )
                )
                state["release"].set()
                mine = client.until(pending)["pending"]
                assert_true(mine.get("route") != "parent", mine)
                client.send("reply", interaction_id=mine["id"], text="y")
                wait_until(lambda: (root / "mine.txt").exists(), "the person's approval was lost")
            finally:
                client.close()
            process.communicate(timeout=budget(20))
        finally:
            state["release"].set()
            process.kill()


def test_an_agent_takes_its_parents_mode_auto_at_most(root, home, *, binary):
    """A yolo parent's agent reviews its own calls (Auto); one the person puts
    in yolo keeps it when its runtime starts again."""
    state = {"holding": threading.Event(), "release": threading.Event(), "modes": []}

    def route(_, body):
        if brief_of(body):
            state["modes"].append(approval_mode(body))
            if len(state["modes"]) == 1:
                state["holding"].set()
                state["release"].wait(budget(20))
            return event({"content": "agent ok"})
        results = results_of(body)
        if not results:
            return tool_call("agent_start", {"brief": "BRIEF hold", "wait": False})
        if len(results) == 1 and '" finished (' in text_of(body):
            # Sent once its runtime is gone, so a new one starts for it.
            wait_until(lambda: not socket_of(home, agent_files(home)[0]).exists(), "still up")
            agent = fnv1a64(str(agent_files(home)[0]))
            return tool_call("agent_send", {"id": agent, "text": "go"}, call_id="call-2")
        return event({"content": "parent ok"})

    with Server([route]) as server:
        process = popen(binary, root, base_env(home, server.url), "--yolo", "-p", "delegate")
        try:
            assert_true(state["holding"].wait(budget(20)), "the agent never ran its brief")
            client = client_of(home, agent_files(home)[0])
            try:
                chosen = client.send("permissions", mode="yolo")
                client.until(
                    lambda f: (
                        f.get("kind") == "outcome" and f.get("request_id") == chosen["request_id"]
                    )
                )
            finally:
                client.close()
            state["release"].set()
            output, errors = process.communicate(timeout=budget(30))
            assert_true(output.strip() == "parent ok", (output, errors))
        finally:
            state["release"].set()
            process.kill()
        assert_true(state["modes"] == ["auto", "yolo"], state["modes"])


def test_an_agent_that_may_not_edit_only_reads(root, home, *, binary):
    """edits none leaves it no tool that edits whatever its settings allow,
    and the agents it starts only read too."""

    def route(_, body):
        if brief_of(body):
            if not results_of(body):
                return tool_call("agent_start", {"brief": "BRIEF deeper", "edits": "folder"})
            return event({"content": "agent ok"})
        if not results_of(body):
            return tool_call("agent_start", {"brief": "BRIEF look", "edits": "none"})
        return event({"content": "parent ok"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_TOOL_CAPABILITIES"] = "inspect,execute,mutate,delegate,external"
        result = run(root, env, "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        agent = next(b for _, b in server.requests if brief_of(b))
        names = function_names(agent)
        assert_true("read_path" in names and "write_file" not in names, names)
        assert_true("edit_file" not in names, names)
        refused = next(results_of(b)[0] for _, b in server.requests if brief_of(b) and results_of(b))
        assert_true("start them with edits none" in refused, refused)
        assert_true(len(agent_files(home)) == 1, agent_files(home))


def test_agents_started_in_a_row_stop_at_three_and_depth_is_bounded(root, home, *, binary):
    """A parent answering every report with another message gets three turns
    of its agent, then is told to ask the person; an agent at agents.depth
    starts none of its own."""

    def route(_, body):
        if brief_of(body):
            assert_true("agent_start" not in function_names(body), function_names(body))
            return event({"content": "agent turn"})
        results = results_of(body)
        if not results:
            return tool_call("agent_start", {"brief": "BRIEF loop", "wait": False})
        if len(results) <= 4 and '" finished (' in text_of(body):
            # Each message waits for the turn the last one started to end, so
            # it starts the next rather than steering that one.
            wait_until(
                lambda: not socket_of(home, agent_files(home)[0]).exists(),
                "the agent never finished its turn",
            )
            agent = fnv1a64(str(agent_files(home)[0]))
            return tool_call(
                "agent_send", {"id": agent, "text": "again"}, call_id=f"call-{len(results)}"
            )
        return event({"content": "parent stops"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_AGENTS_DEPTH"] = "1"
        result = run(root, env, "-p", "loop", timeout=60, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        sends = results_of(server.requests[-1][1])[1:]
        assert_true(sends[:3] == ["sent"] * 3, sends)
        assert_true("3 turns of" in sends[3] and "ask the person" in sends[3], sends)
        assert_true(header(agent_files(home)[0])["agent"]["streak"] == 3, agent_files(home))


def test_escape_stops_the_agents_a_call_waits_for(root, home, *, binary):
    state = {"started": threading.Event(), "stopped": threading.Event()}

    def route(_, body):
        if brief_of(body):
            state["started"].set()
            state["stopped"].wait(budget(30))
            return event({"content": "too late"})
        if not results_of(body):
            return tool_call("agent_start", {"brief": "BRIEF slow"})
        return event({"content": "parent after"})

    with Server([route]) as server:
        # The folder's lead agent waits in a runtime a person can interrupt.
        process = popen(binary, root, base_env(home, server.url), "panel", "-p", "wait")
        try:
            assert_true(state["started"].wait(budget(20)), "the agent never started")
            panel = next((home / ".uagent" / "history").glob("*/panel.json"))
            client = SessionClient(socket_of(home, panel))
            try:
                client.send("interrupt")
            finally:
                client.close()
            agent = agent_files(home)[0]
            # Interrupted, its turn ends and its runtime with it, long before
            # its model answers.
            wait_until(
                lambda: not socket_of(home, agent).exists(), "the agent kept running", timeout=10
            )
            process.communicate(timeout=budget(20))
        finally:
            state["stopped"].set()
            process.kill()


def test_deleting_an_agent_needs_the_person(root, home, *, binary):
    def route(_, body):
        if brief_of(body):
            return event({"content": "agent ok"})
        results = results_of(body)
        if not results:
            return tool_call("agent_start", {"brief": "BRIEF keep me"})
        if len(results) == 1:
            rows = json.loads(results[0])
            return tool_calls(
                [
                    ("list", "agents", {"action": "list"}),
                    ("read", "agents", {"action": "read", "id": rows[0]["id"], "part": "report"}),
                    ("delete", "agents", {"action": "delete", "id": rows[0]["id"]}),
                ]
            )
        return event({"content": "parent ok"})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "-p", "delete it", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        listed, read, deleted = results_of(server.requests[-1][1])[1:]
        (agent,) = agent_files(home)
        assert_true(fnv1a64(str(agent)) in listed and "keep me" in listed, listed)
        assert_true("agent ok" in read, read)
        assert_true("needs a person's approval" in deleted, deleted)
        assert_true(agent.exists(), "the agent was deleted without the person")
