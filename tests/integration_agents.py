import json
import re
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
    save_settings,
    tool_call,
    tool_calls,
    tool_results,
    wait_until,
)
from session_support import SessionClient, runtime_directory


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


def last_results(body):
    return tool_results(body["messages"])


def socket_of(home, path):
    from integration_support import fnv1a64

    return runtime_directory(home) / f"{fnv1a64(str(path))}.sock"


def idle(frame):
    return frame.get("kind") == "state" and frame.get("checkpoint") and not frame.get("busy")


def pending(frame):
    return frame.get("kind") == "state" and frame.get("pending")


def test_a_waited_agent_answers_inline_and_keeps_its_conversation(root, home, *, binary):
    """wait returns the agent's answer as the call's result; the agent is a
    saved conversation beside its parent that agent_send starts again."""

    def route(_, body):
        brief = brief_of(body)
        if brief:
            text = json.dumps(body["messages"])
            return event({"content": "again: ok" if "once more" in text else "agent says 42"})
        results = last_results(body)
        if not results:
            return tool_call("agent_start", {"brief": "BRIEF answer"})
        if len(results) == 1:
            rows = json.loads(results[0])
            return tool_call("agent_send", {"id": rows[0]["id"], "text": "once more"})
        return event({"content": "parent done"})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "-p", "ask an agent", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip().endswith("parent done"), result.stdout)
        open("/private/tmp/claude-501/-Users-timongentzsch-Software-personal-uAgent/a7d1937f-7d02-406f-a429-b37eee902ec6/scratchpad/dbg.json", "w").write(json.dumps([b for _, b in server.requests], indent=1))
        rows = json.loads(
            next(last_results(b)[0] for _, b in server.requests if len(last_results(b)) == 1)
        )
        assert_true(rows[0]["state"] == "completed", rows)
        assert_true("agent says 42" in rows[0]["answer"], rows)
        (agent,) = agent_files(home)
        role = header(agent)["agent"]
        assert_true(role["edits"] == "none" and role["depth"] == 1, role)
        # Its parent's file lies beside it; -c and the resume list skip it.
        parents = [p for p in agent.parent.glob("*.json") if p != agent]
        assert_true(len(parents) == 1, parents)
        # agent_send started a turn on the saved agent, whose report woke the
        # headless parent before it answered.
        wait_until(
            lambda: any("again: ok" in json.dumps(b) for _, b in server.requests),
            "agent_send did not start the agent again",
        )
        assert_true(header(agent)["turns"] == 2, header(agent))
