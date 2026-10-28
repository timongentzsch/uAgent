import json
import pathlib
import sys
import threading
import time

from integration_support import (
    Server,
    assert_true,
    budget,
    event,
    function_names,
    live_process_states,
    tool_call,
    wait_until,
    write_mcp_server,
)
from web_support import web_host


def test_web_mcp_overview_toggles_and_restarts(root, home, *, binary):
    project = root / "mcp-overview"
    project.mkdir()
    fake = project / "fake_mcp.py"
    write_mcp_server(
        fake,
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], 'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'echo', 'inputSchema': {'type': 'object'}}]}\n"
        "    else:\n"
        "        result = {}\n",
    )
    server = {"command": sys.executable, "args": [str(fake)]}
    (project / ".mcp.json").write_text(
        json.dumps(
            {
                "mcpServers": {
                    "probe": server,
                    "broken": {"command": str(project / "missing"), "required": False},
                }
            }
        )
    )
    global_config = home / ".mcp.json"
    global_config.write_text(json.dumps({"mcpServers": {"probe": server, "extra": server}}))

    def status(value):
        return {row["name"]: row for row in value["state"]["mcp"]}

    try:
        with Server([lambda _, _body: event({"content": "unused"})]) as provider:
            with web_host(binary, root, home, provider.url) as (client, code, _, _):
                client.pair(code)
                session = client.create(project)
                value = client.until(session, lambda value: bool(value.get("pending")))
                client.command("reply", session, interaction_id=value["pending"]["id"], text="y")
                value = client.until(
                    session,
                    lambda value: (
                        value["metadata"]["status"] == "idle" and "mcp" in value.get("state", {})
                    ),
                )
                rows = status(value)
                assert_true(rows["probe"]["state"] == "ready", rows)
                assert_true(rows["probe"]["scope"] == "project", rows)
                assert_true(rows["probe"]["overrides"] and rows["probe"]["tools"] == 1, rows)
                assert_true(rows["extra"]["scope"] == "global", rows)
                assert_true(rows["broken"]["state"] == "failed", rows)
                assert_true(rows["broken"]["error"], rows)

                # Disabling edits the project file, stops the server now and
                # keeps the workspace trusted.
                result = client.command(
                    "tools", session, operation="mcp_enable", name="probe", enabled=False
                )
                rows = {row["name"]: row for row in result["result"]["mcp"]}
                assert_true(rows["probe"]["state"] == "disabled", rows)
                assert_true(rows["probe"]["tools"] == 0, rows)
                saved = json.loads((project / ".mcp.json").read_text())
                assert_true(saved["mcpServers"]["probe"]["disabled"] is True, saved)
                again = client.create(project)
                value = client.snapshot(again)
                assert_true(not value.get("pending"), value)

                result = client.command(
                    "tools", session, operation="mcp_enable", name="probe", enabled=True
                )
                rows = {row["name"]: row for row in result["result"]["mcp"]}
                assert_true(rows["probe"]["state"] == "ready", rows)
                assert_true(rows["probe"]["tools"] == 1, rows)
                saved = json.loads((project / ".mcp.json").read_text())
                assert_true("disabled" not in saved["mcpServers"]["probe"], saved)

                result = client.command("tools", session, operation="mcp_restart", name="broken")
                rows = {row["name"]: row for row in result["result"]["mcp"]}
                assert_true(rows["broken"]["state"] == "failed", rows)

                # A global server is switched in the user's own file.
                client.command(
                    "tools", session, operation="mcp_enable", name="extra", enabled=False
                )
                saved = json.loads(global_config.read_text())
                assert_true(saved["mcpServers"]["extra"]["disabled"] is True, saved)
                assert_true("disabled" not in saved["mcpServers"]["probe"], saved)
    finally:
        global_config.unlink(missing_ok=True)


def test_web_restarts_conversations_and_itself(root, home, *, binary):
    project = root / "restarts"
    project.mkdir()
    active = threading.Event()
    finish = threading.Event()
    held = threading.Event()

    def slow(_, _body):
        held.wait(budget(20))
        return event({"content": "Slow answer"})

    def through_restart(_, _body):
        active.set()
        finish.wait(budget(20))
        return event({"content": "Survived host restart"})

    with Server(
        [lambda _, _body: event({"content": "Remembered answer"}), slow, through_restart]
    ) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, process, _):
            client.pair(code)
            session = client.create(project)
            client.command("submit", session, text="Remember this")
            value = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and "Remembered answer" in json.dumps(value)
                ),
            )
            generation = value["metadata"]["generation"]

            # An idle conversation gets a fresh runtime that keeps its history.
            result = client.command("restart_conversations")["result"]
            assert_true(result == {"restarting": 1, "deferred": 0}, result)
            value = client.until(
                session,
                lambda value: (
                    value["metadata"].get("generation") not in ("", generation)
                    and value["metadata"]["status"] == "idle"
                ),
            )
            assert_true("Remembered answer" in json.dumps(value), value)
            other = client.command("restart_conversations", cwd=str(root / "elsewhere"))
            assert_true(other["result"] == {"restarting": 0, "deferred": 0}, other)
            # One conversation by id (the web's Restart): another id leaves it.
            other = client.command("restart_conversations", target_id="no-such-session")
            assert_true(other["result"] == {"restarting": 0, "deferred": 0}, other)
            generation = value["metadata"]["generation"]
            one = client.command("restart_conversations", target_id=session["id"])
            assert_true(one["result"] == {"restarting": 1, "deferred": 0}, one)
            value = client.until(
                session,
                lambda value: (
                    value["metadata"].get("generation") not in ("", generation)
                    and value["metadata"]["status"] == "idle"
                ),
            )

            # A busy conversation finishes its turn first, then restarts.
            generation = value["metadata"]["generation"]
            session = value["metadata"]
            client.command("submit", session, text="Take your time")
            client.until(session, lambda value: value["metadata"]["turn_active"])
            result = client.command("restart_conversations")["result"]
            held.set()
            assert_true(result == {"restarting": 0, "deferred": 1}, result)
            value = client.until(
                session,
                lambda value: (
                    value["metadata"].get("generation") not in ("", generation)
                    and value["metadata"]["status"] == "idle"
                ),
            )
            assert_true("Slow answer" in json.dumps(value), value)

            # The host re-execs in place: same process, new epoch, same pairing.
            session = value["metadata"]
            generation = value["metadata"]["generation"]
            client.command("submit", session, text="Continue through the host restart")
            assert_true(active.wait(budget(10)), "turn did not start")
            discovery = home / ".uagent/web/discovery.json"
            epoch = json.loads(discovery.read_text())["epoch"]
            client.command("restart_host")

            def restarted():
                try:
                    current = json.loads(discovery.read_text())["epoch"]
                    return current != epoch and client.json("/api/sessions")[0] == 200
                except (OSError, ValueError, ConnectionError):
                    return False

            wait_until(restarted, "web host did not come back", timeout=20)
            assert_true(process.poll() is None, "host exited instead of re-exec")
            try:
                client.until(
                    session,
                    lambda value: (
                        value["metadata"].get("generation") == generation
                        and value["metadata"]["turn_active"]
                    ),
                )
            finally:
                finish.set()
            value = client.until(session, lambda value: not value["metadata"]["turn_active"])
            assert_true("Survived host restart" in json.dumps(value), value)


def test_web_conversation_starts_in_a_space_by_name(root, home, *, binary):
    """A name instead of a directory: the host keeps a folder for it under
    ~/.uagent/spaces, and the same name is the same folder."""
    with Server([event({"content": "ok"})]) as provider:
        with web_host(binary, root, home, provider.url) as (web, code, _, _env):
            web.pair(code)
            first = web.command("create", space="notes-1")["session"]
            again = web.command("create", space="Notes-1")["session"]
            space = (home / ".uagent" / "spaces" / "notes-1").resolve()
            assert_true(space.is_dir(), "the workspace was not created")
            assert_true(first["cwd"] == again["cwd"] == str(space), (first, again))
            assert_true(first["id"] != again["id"], "a second conversation was not started")
            for number, name in enumerate(("../outside", "a/b", "x" * 65)):
                status, refused, _ = web.json(
                    "/api/command",
                    {
                        "v": 2,
                        "kind": "create",
                        "request_id": f"{0xE0 + number:032x}",
                        "space": name,
                    },
                )
                assert_true(
                    status == 409 and "workspace's name" in refused["error"], (name, refused)
                )
            assert_true(not (home / ".uagent" / "outside").exists(), "a name left its folder")


def test_web_space_is_renamed_and_deleted(root, home, *, binary):
    """A space goes by the name it was given, in several words if it likes.
    Renaming it changes that name and nothing else; deleting it removes its
    conversations and its folder, and never a folder that is not a workspace."""
    with Server([event({"content": "ok"})]) as provider:
        with web_host(binary, root, home, provider.url) as (web, code, _, _env):
            web.pair(code)
            session = web.command("create", space="Trip to Rome")["session"]
            space = pathlib.Path(session["cwd"])
            assert_true(space.parent == (home / ".uagent" / "spaces").resolve(), space)
            other = web.command("create", space="other")["session"]

            def spaces():
                return {s["name"]: s["path"] for s in web.json("/api/sessions")[1]["spaces"]}

            assert_true(spaces() == {"other": other["cwd"], "Trip to Rome": str(space)}, spaces())

            renamed = web.command("space", action="rename", cwd=str(space), name="Rome 2027")
            assert_true(
                [s["name"] for s in renamed["result"]["spaces"]] == ["other", "Rome 2027"], renamed
            )
            same = web.command("create", space="rome 2027")["session"]
            assert_true(same["cwd"] == str(space), "the renamed space is another folder")
            listed = {s["id"]: s["cwd"] for s in web.json("/api/sessions")[1]["sessions"]}
            assert_true(listed.get(session["id"]) == str(space), "a conversation changed")
            for number, fields in enumerate(
                (
                    {"action": "rename", "cwd": str(space), "name": "other"},
                    {"action": "rename", "cwd": str(space), "name": "a/b"},
                    {"action": "rename", "cwd": str(root), "name": "mine"},
                    {"action": "delete", "cwd": str(root)},
                    {"action": "delete", "cwd": str(space.parent)},
                )
            ):
                status, refused, _ = web.json(
                    "/api/command",
                    {"v": 2, "kind": "space", "request_id": f"{0xF0 + number:032x}", **fields},
                )
                assert_true(status == 409, (fields, refused))
            assert_true(root.is_dir() and space.is_dir(), "a refused command removed a folder")

            # One of its conversations is running: it is closed first.
            (space / "notes.txt").write_text("keep?")
            running = web.command("activate", session)["session"]
            assert_true(running.get("presence") == "active", running)
            deleted = web.command("space", action="delete", cwd=str(space))
            assert_true([s["name"] for s in deleted["result"]["spaces"]] == ["other"], deleted)
            assert_true(not space.exists(), "the workspace's folder is still there")
            left = [s["cwd"] for s in web.json("/api/sessions")[1]["sessions"]]
            assert_true(left == [other["cwd"]], left)


def test_folder_panel_is_one_session_with_its_own_tools(root, home, *, binary):
    project = root / "coordinated"
    project.mkdir()
    with Server([event({"content": "panel-ok"})]) as provider:
        with web_host(binary, root, home, provider.url) as (web, code, _, _env):
            web.pair(code)
            first = web.command("create", cwd=str(project), panel=True)["session"]
            again = web.command("create", cwd=str(project), panel=True)["session"]
            assert_true(first["id"] == again["id"], (first, again))
            assert_true(first["kind"] == "panel", first)
            session = web.command("activate", first)["session"]
            web.until(session, lambda value: value["metadata"]["status"] == "idle")
            web.command("submit", session, text="status?")
            web.until(
                session,
                lambda value: any(
                    block.get("body", {}).get("text") == "panel-ok"
                    for block in value["state"]["view"]["blocks"]
                ),
            )
            _, body = provider.requests[-1]
            names = {tool["function"]["name"] for tool in body.get("tools", [])}
            assert_true("history" in names and "write_file" not in names, names)
            _, catalogue, _ = web.json("/api/sessions?refresh=1")
            listed = catalogue["sessions"]
            kinds = [item.get("kind") for item in listed if item["cwd"] == str(project.resolve())]
            assert_true(kinds == ["panel"], listed)


def test_idle_panel_is_let_go_and_exits(root, home, *, binary):
    from session_support import runtime_directory

    project = root / "idle"
    project.mkdir()
    with Server([event({"content": "ok"})]) as provider:
        with web_host(
            binary, root, home, provider.url, extra_env={"UAGENT_INTERNAL_IDLE_S": "2"}
        ) as (web, code, _, _env):
            web.pair(code)
            session = web.command("create", cwd=str(project), panel=True)["session"]
            web.command("activate", session)

            def sockets():
                return list(runtime_directory(home).glob("*.sock"))

            wait_until(sockets, "panel runtime never started", timeout=10)
            # The host lets go after the idle period; the runtime then exits.
            wait_until(lambda: not sockets(), "idle panel kept running", timeout=30)
            again = web.command("create", cwd=str(project), panel=True)["session"]
            assert_true(again["id"] == session["id"], again)
            web.command("activate", again)
            wait_until(sockets, "panel did not start again", timeout=10)


def test_idle_session_stops_and_a_message_starts_it_again(root, home, *, binary):
    from session_support import runtime_directory

    project = root / "idle-session"
    project.mkdir()
    with Server([event({"content": "first-ok"}), event({"content": "second-ok"})]) as provider:
        with web_host(
            binary, root, home, provider.url, extra_env={"UAGENT_INTERNAL_IDLE_S": "2"}
        ) as (web, code, _, _env):
            web.pair(code)
            session = web.create(project)
            web.command("permissions", session, mode="yolo")
            web.command("submit", session, text="one")
            web.until(session, lambda value: "first-ok" in json.dumps(value))

            def sockets():
                return list(runtime_directory(home).glob("*.sock"))

            wait_until(lambda: not sockets(), "idle session kept running", timeout=30)
            saved = web.until(session, lambda value: value["metadata"]["status"] == "saved")
            assert_true("first-ok" in json.dumps(saved), saved)
            # No resume step: a command for the runtime starts it, a setting
            # as much as a message.
            web.command("model", saved["metadata"], operation="catalog")
            started = web.until(session, lambda value: value["metadata"]["generation"])
            web.command("submit", started["metadata"], text="two")
            again = web.until(session, lambda value: "second-ok" in json.dumps(value))
            # The same session: its settings came back with it.
            assert_true(again["state"]["permissions"]["mode"] == "yolo", again["state"])


def test_a_subagent_gets_no_tool_its_parent_switched_off(root, home, *, binary):
    project = root / "tools-project"
    project.mkdir()
    offered = {}

    def route(_, body):
        users = [str(m.get("content", "")) for m in body["messages"] if m.get("role") == "user"]
        if "go" not in users:
            offered["child"] = function_names(body)
            return event({"content": "child-done"})
        if any(m.get("role") == "tool" for m in body["messages"]):
            return event({"content": "parent-done"})
        offered["parent"] = function_names(body)
        return tool_call("subagent", {"prompt": "child-task", "mode": "full", "background": True})

    with Server([route] * 8) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("permissions", session, mode="yolo")
            client.command("tools", session, operation="set", name="write_file", active=False)
            client.command("submit", session, text="go")

            def ceiling_kept(answers):
                done = client.until(
                    session,
                    # Idle again once the parent has answered the result: the
                    # turn that takes it up has ended, not merely begun. Idle
                    # alone may be the moment before that turn starts.
                    lambda value: (
                        "child" in offered
                        and value["metadata"]["status"] == "idle"
                        and all(
                            row["status"] == "completed" for row in value["state"]["activities"]
                        )
                        and sum(
                            block.get("role") == "assistant"
                            and block.get("body", {}).get("text") == "parent-done"
                            for block in value["state"].get("view", {}).get("blocks", [])
                        )
                        >= answers
                    ),
                )
                assert_true("write_file" not in offered["parent"], offered["parent"])
                # The child's full toolset, less what its parent may not call.
                child = offered.pop("child")
                assert_true("write_file" not in child and "read_path" in child, child)
                return done["state"]["activities"][0]["agent_id"]

            child = ceiling_kept(2)
            # A person's follow-up to it is no decision about tools either.
            client.command(
                "activity", session, operation="followup", agent_id=child, text="child-task more"
            )
            ceiling_kept(3)


def test_web_interrupt_stops_a_command_that_ignores_being_asked(root, home, *, binary):
    """Stop asks a running command to end, then ends it: the turn does not
    wait out a command that ignores the request."""
    project = root / "interrupt-project"
    project.mkdir()
    started = project / "started"
    command = f"trap '' TERM; echo $$ > {started}; sleep 30 & wait"
    with Server(
        [tool_call("run", {"command": command, "yield_ms": 0}), event({"content": "after"})]
    ) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("permissions", session, mode="yolo")
            client.command("submit", session, text="go")
            wait_until(started.exists, "the command never started", timeout=10)
            shell = int(started.read_text())
            asked = time.monotonic()
            client.command("interrupt", client.snapshot(session)["metadata"])
            client.until(session, lambda value: not value["metadata"]["turn_active"])
            assert_true(time.monotonic() - asked < budget(8), "the interrupt waited on the command")
            assert_true(not live_process_states([shell]), "the command outlived its interrupt")


def test_stopped_session_starts_for_several_commands_and_says_why_it_cannot(root, home, *, binary):
    import shutil

    project = root / "restart-project"
    project.mkdir()
    with Server([event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (web, code, _, _env):
            web.pair(code)
            session = web.create(project)

            def stop():
                live = web.until(session, lambda value: value["metadata"]["generation"])
                web.command("close", live["metadata"])
                return web.until(session, lambda value: value["metadata"]["status"] == "saved")

            def send(request, kind, **values):
                return web.json(
                    "/api/command",
                    {
                        "v": 2,
                        "kind": kind,
                        "request_id": request * 32,
                        "session_id": session["id"],
                        "generation": "",
                        **values,
                    },
                )

            stop()
            # A settings screen opens with several commands at once: each
            # waits for the one start instead of being refused during it.
            outcomes = []
            threads = [
                threading.Thread(target=lambda r=r, k=k, v=v: outcomes.append(send(r, k, **v)))
                for r, k, v in (
                    ("a", "model", {"operation": "catalog"}),
                    ("b", "tools", {"operation": "catalog"}),
                    ("c", "permissions", {}),
                )
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            assert_true(
                all(status == 200 and body.get("accepted") for status, body, _ in outcomes),
                [body for _, body, _ in outcomes],
            )

            stop()
            # A conversation whose folder is gone cannot run: it says so at
            # once, not after a wait for a runtime that already ended.
            shutil.rmtree(project)
            started = time.monotonic()
            status, body, _ = send("d", "model", operation="catalog")
            assert_true(
                status == 409
                and "folder no longer exists" in body.get("error", "")
                and time.monotonic() - started < 2,
                (status, body, time.monotonic() - started),
            )


def test_panel_edit_from_here_rewinds_in_place(root, home, *, binary):
    project = root / "rewind"
    project.mkdir()
    with Server([event({"content": "one-ok"}), event({"content": "two-ok"})]) as provider:
        with web_host(binary, root, home, provider.url) as (web, code, _, _env):
            web.pair(code)
            session = web.command("create", cwd=str(project), panel=True)["session"]
            session = web.command("activate", session)["session"]
            web.until(session, lambda value: value["metadata"]["status"] == "idle")
            for text, answer in (("first", "one-ok"), ("second", "two-ok")):
                web.command("submit", session, text=text)
                web.until(
                    session,
                    lambda value, answer=answer: (
                        value["metadata"]["status"] == "idle"
                        and any(
                            block.get("body", {}).get("text") == answer
                            for block in value["state"]["view"]["blocks"]
                        )
                    ),
                )
            rewound = web.command("fork", session, turn=2)["result"]
            assert_true(rewound.get("rewound") and rewound["id"] == session["id"], rewound)
            assert_true(rewound["prompt"] == "second", rewound)
            view = web.until(
                session,
                lambda value: (
                    not any(
                        block.get("body", {}).get("text") in ("second", "two-ok")
                        for block in value["state"]["view"]["blocks"]
                    )
                ),
            )
            users = [
                b["body"]["text"] for b in view["state"]["view"]["blocks"] if b["kind"] == "turn"
            ]
            assert_true(users == ["first"], users)
