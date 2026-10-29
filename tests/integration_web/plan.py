import json
import re

from integration_support import (
    Server,
    assert_true,
    event,
    tool_call,
    tool_calls,
    tool_results,
    wait_until,
)
from session_support import runtime_directory
from web_support import web_host

PROMPTS = ("Plan the change", "Add tests", "The plan is approved", "Just answer", "Plan again")


def plan_path(messages):
    """Where plan mode's context tells the model to write the plan."""
    found = re.findall(r"write the plan \(markdown\) to (\S+?);", json.dumps(messages))
    return found[-1] if found else ""


def last_prompt(messages):
    """The latest of PROMPTS the person sent, and the tool results since."""
    for index in range(len(messages) - 1, -1, -1):
        if messages[index].get("role") != "user":
            continue
        text = json.dumps(messages[index].get("content"))
        for prompt in PROMPTS:
            if prompt in text:
                return prompt, tool_results(messages[index + 1 :])
    return "", []


def settled(marker, count=1):
    return lambda value: (
        json.dumps(value).count(marker) >= count
        and not value["metadata"]["turn_active"]
        and not value.get("pending")
    )


def test_plan_mode_reads_then_puts_its_plan_to_the_person(root, home, *, binary):
    """In plan mode only reads and the plan file are allowed; each turn ends
    with the plan shared and three choices: keep planning starts a planning
    turn with the feedback, either approval leaves plan mode in that mode and
    starts the work. A turn that writes no plan ends with a notice."""
    project = root / "plan-project"
    project.mkdir()
    kept = project / "keep.txt"
    kept.write_text("before\n")
    made = project / "made.txt"
    told = []

    def responder(_, body):
        messages = body["messages"]
        prompt, results = last_prompt(messages)
        told.extend(results)
        if prompt == "The plan is approved":
            return event({"content": "CARRIED-OUT"})
        if prompt == "Just answer":
            return event({"content": "NO-PLAN-NEEDED"})
        plan = plan_path(messages)
        if prompt == "Plan the change" and not results:
            return tool_calls(
                [
                    ("edit", "write_file", {"path": "keep.txt", "content": "x", "overwrite": True}),
                    ("touch", "run", {"command": f"touch {made}"}),
                ]
            )
        if len(results) == (2 if prompt == "Plan the change" else 0):
            text = "# Plan\n1. Change keep.txt\n" + (
                "2. Add tests\n" if prompt == "Add tests" else ""
            )
            return tool_call("write_file", {"path": plan, "content": text, "overwrite": True})
        return event({"content": f"PLANNED {prompt}"})

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("permissions", session, mode="plan")
            client.command("submit", session, text="Plan the change")
            first = client.until(session, lambda value: bool(value.get("pending")))
            # Refused before running: the edit, and the command by the
            # readonly sandbox or, without one, by plan mode.
            assert_true(
                any("plan mode is read-only; write the plan to" in result for result in told),
                told,
            )
            assert_true(kept.read_text() == "before\n" and not made.exists(), told)
            pending = first["pending"]
            assert_true(
                [option["value"] for option in pending["options"]] == ["y", "c", "guidance"],
                pending,
            )
            assert_true([file["name"] for file in pending["files"]] == ["plan.md"], pending)
            assert_true("1. Change keep.txt" in pending["description"], pending)
            shared = [
                part
                for node in first["state"]["view"]["nodes"]
                for part in node.get("parts") or []
                if part["kind"] == "file"
            ]
            assert_true([part["name"] for part in shared] == ["plan.md"], shared)
            status, body, _ = client.request(
                f"/api/sessions/{session['id']}/assets/{shared[0]['id']}"
            )
            assert_true(status == 200 and b"1. Change keep.txt" in body, body)

            # Keep planning: the feedback starts a planning turn.
            client.command("reply", session, interaction_id=pending["id"], text="Add tests")
            second = client.until(
                session,
                lambda value: (
                    (value.get("pending") or {}).get("id", pending["id"]) != pending["id"]
                ),
            )
            assert_true(second["state"]["permissions"]["effective"] == "plan", second["state"])
            assert_true("2. Add tests" in second["pending"]["description"], second["pending"])

            # Approve and run in auto.
            client.command("reply", session, interaction_id=second["pending"]["id"], text="y")
            done = client.until(session, settled("CARRIED-OUT"))
            assert_true(done["state"]["permissions"]["effective"] == "auto", done["state"])
            approved = [
                body
                for _, body in provider.requests
                if "The plan is approved; carry it out" in json.dumps(body["messages"])
            ]
            text = json.dumps(approved[0]["messages"])
            assert_true(re.findall(r"; approval (\w+)", text)[-1] == "auto", text)
            assert_true("carry it out. It is at " in text and ".plan.md." in text, text)

            # Entered again, a turn that writes no plan says so and stays.
            client.command("permissions", session, mode="plan")
            client.command("submit", session, text="Just answer")
            quiet = client.until(session, settled("NO-PLAN-NEEDED"))
            notices = [
                node["headline"]["verb"]
                for node in quiet["state"]["view"]["nodes"]
                if node["kind"] == "notice"
            ]
            assert_true(notices == ["Shared", "Shared", "No plan written"], notices)
            assert_true(quiet["state"]["permissions"]["effective"] == "plan", quiet["state"])

            # Approve and ask before changes.
            client.command("submit", session, text="Plan again")
            third = client.until(session, lambda value: bool(value.get("pending")))
            client.command("reply", session, interaction_id=third["pending"]["id"], text="c")
            asked = client.until(session, settled("CARRIED-OUT", 2))
            assert_true(asked["state"]["permissions"]["effective"] == "ask", asked["state"])


def test_plan_mode_survives_a_restart(root, home, *, binary):
    project = root / "plan-restart"
    project.mkdir()
    with Server([event({"content": "first-ok"}), event({"content": "second-ok"})]) as provider:
        with web_host(
            binary, root, home, provider.url, extra_env={"UAGENT_INTERNAL_IDLE_S": "2"}
        ) as (web, code, _, _env):
            web.pair(code)
            session = web.create(project)
            web.command("permissions", session, mode="plan")
            web.command("submit", session, text="one")
            web.until(session, settled("first-ok"))

            def sockets():
                return list(runtime_directory(home).glob("*.sock"))

            wait_until(lambda: not sockets(), "idle session kept running", timeout=30)
            saved = web.until(session, lambda value: value["metadata"]["status"] == "saved")
            web.command("model", saved["metadata"], operation="catalog")
            started = web.until(session, lambda value: value["metadata"]["generation"])
            web.command("submit", started["metadata"], text="two")
            again = web.until(session, settled("second-ok"))
            assert_true(again["state"]["permissions"]["mode"] == "plan", again["state"])
            assert_true(plan_path(provider.requests[-1][1]["messages"]), provider.requests[-1])
