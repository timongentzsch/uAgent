"""The uagent tool's command action: a slash command the agent asks for runs
when its turn ends, once a person approved the line."""

import contextlib
import json
import threading

from integration_agents import approval_mode
from integration_support import (
    Server,
    assert_true,
    budget,
    config_skill_env,
    event,
    function_tools,
    open_config_skill,
    tool_call,
    tool_results,
    wait_until,
    write_json_response,
)
from web_support import web_host

from integration_web.plan import last_prompt, settled


def ask(line):
    return tool_call("uagent", {"action": "command", "line": line}, call_id="call-command")


def offered_actions(body):
    uagent = next(tool for tool in function_tools(body) if tool["name"] == "uagent")
    return uagent["parameters"]["properties"]["action"]["enum"]


@contextlib.contextmanager
def conversation(binary, root, home, turns):
    """A conversation on the web host with the config skill in reach. Its
    model answers a turn with that turn's steps in `turns`, keyed by words of
    the message that started it; a step is a response or a function of the
    request, and each but the last makes one tool call."""

    def route(handler, body):
        words, results = last_prompt(body["messages"], turns)
        step = turns[words][len(results)]
        return step(handler, body) if callable(step) else step

    with Server([route]) as provider:
        skills = config_skill_env({})
        with web_host(binary, root, home, provider.url, extra_env=skills) as (client, code, _, _):
            client.pair(code)
            yield client, client.create(root)


def decide(client, session, answer="y"):
    """Answers the decision the conversation waits on, and returns it."""
    waiting = client.until(session, lambda value: bool(value.get("pending")))["pending"]
    client.command("reply", session, interaction_id=waiting["id"], text=answer)
    return waiting


def mode_of(value):
    return value["state"]["permissions"]["effective"]


def journal(home):
    """The events of the one conversation of `home`, saved as it closed."""
    (saved,) = (home / ".uagent" / "history").rglob("*.events.jsonl")
    return saved.read_text()


def test_an_approved_command_runs_when_the_turn_ends(root, home, *, binary):
    """The person approves the exact line, in every approval mode; the model
    is told only that it is queued, and the mode it asked for holds once the
    turn is over. A line the host does not run is refused before anyone is
    asked, and a refused approval runs nothing."""
    seen = {}

    def tell(_, body):
        seen["result"] = tool_results(body["messages"])[-1]
        seen["mode"] = approval_mode(body)
        return event({"content": "asked-ok"})

    def refuse(_, body):
        assert_true("command" in offered_actions(body), offered_actions(body))
        return ask("/review the changes")

    def refusal(_, body):
        seen["refused"] = tool_results(body["messages"])[-1]
        return event({"content": "refused-ok"})

    def denied(_, body):
        seen["denied"] = tool_results(body["messages"])[-1]
        # What the first command printed went to the person, not here.
        assert_true('"effective"' not in json.dumps(body["messages"]), body["messages"])
        return event({"content": "denied-ok"})

    turns = {
        "refuse": [open_config_skill, refuse, refusal],
        "widen": [ask("/permissions yolo"), tell],
        "narrow": [ask("/permissions ask"), denied],
    }
    with conversation(binary, root, home, turns) as (client, session):
        client.command("submit", session, text="refuse")
        client.until(session, settled("refused-ok"))
        assert_true("only starts a turn: do that work yourself" in seen["refused"], seen)

        connection, response = client.events()
        client.command("submit", session, text="widen")
        waiting = decide(client, session)
        assert_true(waiting["approval"]["mandatory_human"], waiting)
        assert_true(
            waiting["approval"]["mandatory_reason"] == "runs a slash command for you", waiting
        )
        assert_true(
            waiting["approval"]["preview"]
            == "/permissions yolo\nThis widens what the agent may do without asking.",
            waiting,
        )
        done = client.until(session, settled("asked-ok"))
        assert_true(seen["result"].startswith("queued: ") and seen["mode"] == "ask", seen)
        assert_true("goes to the user, not to you" in seen["result"], seen)
        assert_true(mode_of(done) == "yolo", done["state"]["permissions"])
        # What it printed reached the clients, as a typed command's does.
        frames = (json.loads(line[6:]) for line in response if line.startswith(b"data: "))
        ran = next(frame["data"] for frame in frames if frame.get("type") == "command.completed")
        connection.close()
        assert_true(ran["command"] == "/permissions" and ran["argument"] == "yolo", ran)
        assert_true(ran["result"]["effective"] == "yolo", ran)

        # Yolo answers no approval of this kind, and "no" queues nothing.
        client.command("submit", session, text="narrow")
        waiting = decide(client, session, "n")
        assert_true("This narrows what" in waiting["approval"]["preview"], waiting)
        done = client.until(session, settled("denied-ok"))
        assert_true("queued" not in seen["denied"], seen)
        assert_true(mode_of(done) == "yolo", done["state"]["permissions"])


def test_messages_queued_for_after_the_turn_run_before_the_command(root, home, *, binary):
    seen = {}

    def follow(_, body):
        seen["mode"] = approval_mode(body)
        return event({"content": "followed-ok"})

    turns = {
        "switch": [open_config_skill, ask("/permissions yolo"), event({"content": "switched"})],
        "follow": [follow],
    }
    with conversation(binary, root, home, turns) as (client, session):
        client.command("submit", session, text="switch")
        waiting = client.until(session, lambda value: bool(value.get("pending")))
        client.command("steer", session, text="follow up", queue=True)
        decide(client, session)
        done = client.until(session, settled("followed-ok"))
        assert_true(seen["mode"] == "ask", seen)
        assert_true(mode_of(waiting) == "ask" and mode_of(done) == "yolo", done["state"])


def test_a_pending_wake_turn_runs_after_the_command(root, home, *, binary):
    """Background work that ends as the turn does starts a turn of its own,
    and the command has run by then."""
    release, ended = root / "release", root / "ended"
    seen = {}

    def answer(_, __):
        # The last request of the turn is out: the work ends too late for it.
        release.touch()
        wait_until(ended.exists, "the background command never ended")
        return event({"content": "switched"})

    def woken(_, body):
        seen["mode"] = approval_mode(body)
        return event({"content": "woken-ok"})

    waits = f"until [ -e {release} ]; do sleep 0.05; done; touch {ended}"
    turns = {
        "switch": [
            open_config_skill,
            tool_call("run", {"command": waits, "yield_ms": 250}),
            ask("/permissions ask"),
            answer,
        ],
        "activity finished": [woken],
    }
    with conversation(binary, root, home, turns) as (client, session):
        client.command("permissions", session, mode="yolo")
        client.command("submit", session, text="switch")
        decide(client, session)
        done = client.until(session, settled("woken-ok"))
        assert_true(seen["mode"] == "ask" and mode_of(done) == "ask", (seen, done["state"]))


def test_a_turn_that_does_not_complete_drops_its_command(root, home, *, binary):
    """Approved, the command still waits for the turn to end well: one that
    failed or was stopped takes it along, and says so."""
    holding, release = threading.Event(), threading.Event()

    def fail(handler, _):
        write_json_response(handler, {"error": {"message": "terminal rejection"}}, status=400)

    def hold(_, __):
        holding.set()
        release.wait(budget(20))
        return event({"content": "too late"})

    turns = {
        "fail": [open_config_skill, ask("/permissions yolo"), fail],
        "stop": [ask("/permissions yolo"), hold],
    }

    def ended(reason):
        return lambda value: (
            not value["metadata"]["turn_active"]
            and (value["state"].get("stop") or {}).get("reason") == reason
        )

    with conversation(binary, root, home, turns) as (client, session):
        try:
            client.command("submit", session, text="fail")
            decide(client, session)
            done = client.until(session, ended("error"))
            assert_true(mode_of(done) == "ask", done["state"]["permissions"])

            client.command("submit", session, text="stop")
            decide(client, session)
            assert_true(holding.wait(budget(20)), "the turn never reached its answer")
            client.command("interrupt", session)
            done = client.until(session, ended("cancelled"))
            assert_true(mode_of(done) == "ask", done["state"]["permissions"])
        finally:
            release.set()
    events = journal(home)
    assert_true(events.count("not run, the turn did not complete: /permissions yolo") == 2, events)


def test_a_command_leaves_running_agents_as_they_are(root, home, *, binary):
    """An agent is offered no command, and one that runs while its parent's
    command takes effect keeps the mode it has."""
    holding, release = threading.Event(), threading.Event()
    modes, seen = [], {}

    def held(handler, body):
        modes.append(approval_mode(body))
        holding.set()
        release.wait(budget(20))
        return open_config_skill(handler, body)

    def tries(_, body):
        seen["actions"] = offered_actions(body)
        return ask("/permissions yolo")

    def reports(_, body):
        modes.append(approval_mode(body))
        seen["refused"] = tool_results(body["messages"])[-1]
        return event({"content": "agent ok"})

    turns = {
        # The agent's report names its brief, so it is looked for first.
        "finished (": [event({"content": "heard-ok"})],
        "BRIEF": [held, tries, reports],
        "delegate": [
            open_config_skill,
            tool_call("agent_start", {"brief": "BRIEF hold", "wait": False}),
            ask("/permissions yolo"),
            event({"content": "delegated"}),
        ],
    }
    with conversation(binary, root, home, turns) as (client, session):
        try:
            client.command("submit", session, text="delegate")
            assert_true(holding.wait(budget(20)), "the agent never ran its brief")
            decide(client, session)
            done = client.until(session, settled("delegated"))
            assert_true(mode_of(done) == "yolo", done["state"]["permissions"])
            release.set()
            client.until(session, settled("heard-ok"))
        finally:
            release.set()
    assert_true(seen["actions"] == ["show", "set", "write"], seen)
    assert_true("invalid tool argument" in seen["refused"], seen)
    assert_true(len(modes) == 2 and modes[0] == modes[1] != "yolo", modes)
