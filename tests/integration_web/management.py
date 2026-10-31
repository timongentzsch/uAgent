import json
import signal
import threading
from pathlib import Path

from integration_support import (
    Server,
    assert_true,
    budget,
    event,
    fnv1a64,
    save_settings,
    session_files,
    tool_call,
    wait_until,
    write_session,
)
from web_support import web_host

from integration_web._common import PNG


def test_web_conversation_management_and_statistics(root, home, *, binary):
    import fcntl

    project = root / "shared-project"
    project.mkdir()
    sentinel = project / "keep.txt"
    sentinel.write_text("project files survive")
    usage = {
        "prompt_tokens": 120,
        "completion_tokens": 40,
        "completion_tokens_details": {"reasoning_tokens": 10},
        "prompt_tokens_details": {"cached_tokens": 20},
    }
    with Server([lambda _, _body: event({"content": "Recorded answer"}, usage=usage)]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            draft = client.create(project, activate=False)
            client.command("rename", draft, title="Before activation")
            assert_true(client.snapshot(draft)["metadata"]["title"] == "Before activation", draft)
            session = client.command("activate", draft)["session"]
            client.until(session, lambda value: value["metadata"]["status"] == "idle")
            client.command("rename", session, title="test")
            client.until(session, lambda value: value["metadata"]["title"] == "test")
            client.command("close", session)
            saved = client.snapshot(session)["metadata"]
            assert_true(saved["title"] == "test", saved)
            previous_generation = session["generation"]
            session = client.command("activate", saved)["session"]
            assert_true(session["generation"] != previous_generation, "rejoined a closing runtime")
            client.until(session, lambda value: value["metadata"]["status"] == "idle")
            endpoint = f"/api/sessions/{session['id']}/attachments"
            status, image, _ = client.json(endpoint, raw=PNG, headers={"Content-Type": "image/png"})
            assert_true(status == 200, image)
            client.command(
                "submit",
                session,
                text="This must not replace my custom title",
                attachment_ids=[image["id"]],
            )
            value = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle" and value["state"].get("turns") == 1
                ),
            )
            assert_true(value["metadata"]["title"] == "test", value["metadata"])
            facts = value["state"]["view"]["nodes"]
            assert_true(all(node.get("time") for node in facts), facts)
            answer = next(node for node in facts if node.get("role") == "assistant")
            assert_true(answer["route"] and answer["ttft_ms"] >= 0, answer)
            assert_true(
                answer["usage_reported"]
                and answer["usage"]["output"] == 30
                and answer["usage"]["reasoning"] == 10,
                answer,
            )
            assert_true(answer["tokens_per_second"] > 0, answer)
            stats = value["state"]["statistics"]
            assert_true(
                stats["complete"]
                and stats["model_calls"] == 1
                and stats["recorded_turns"] == 1
                and stats["ttft_samples"] == 1,
                stats,
            )
            # Changing the next route never relabels the saved response.
            client.command("submit", session, text="/effort low")
            client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle" and value["state"].get("effort") == "low"
                ),
            )
            assert_true(
                next(
                    node
                    for node in client.snapshot(session)["state"]["view"]["nodes"]
                    if node.get("role") == "assistant"
                )
                == answer,
                "historical model changed",
            )

            def rejected(kind, target, **fields):
                client.sequence += 1
                return client.json(
                    "/api/command",
                    {
                        "v": 2,
                        "request_id": f"{client.sequence:032x}",
                        "kind": kind,
                        "session_id": target["id"],
                        "generation": target.get("generation", ""),
                        **fields,
                    },
                )

            assert_true(rejected("delete", session)[0] == 409, "deleted a live worker")
            client.command("close", session)
            saved = client.snapshot(session)["metadata"]
            assert_true(
                client.snapshot(saved)["state"]["statistics"] == stats,
                "statistics changed on reopen",
            )
            assert_true(
                rejected("rename", saved, title="bad\nname")[0] == 409, "accepted control character"
            )
            marker = home / f".uagent/web/drafts/{session['id']}.json"
            path = Path(json.loads(marker.read_text())["path"])
            # The conversation writer lease still excludes a second writer.
            with Path(str(path) + ".lock").open("r+") as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                assert_true(
                    rejected("rename", saved, title="locked")[0] == 409, "writer lease bypassed"
                )
                assert_true(
                    rejected("delete", saved)[0] == 409, "deleted another writer's conversation"
                )
                assert_true(path.exists(), "locked history removed")
            client.command("rename", saved, title="Saved name")
            renamed = client.snapshot(saved)
            assert_true(renamed["metadata"]["title"] == "Saved name", renamed["metadata"])
            assert_true(renamed["state"]["statistics"] == stats, renamed["state"])
        # Restart the master: the title, metrics, and source image all survive.
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            saved = client.snapshot(saved)["metadata"]
            assert_true(saved["title"] == "Saved name", saved)
            assert_true(
                client.snapshot(saved)["state"]["statistics"] == stats, "restart lost stats"
            )
            client.command("delete", saved)
            assert_true(
                client.json(f"/api/sessions/{saved['id']}")[0] == 404,
                "deleted history remained visible",
            )
            assert_true(
                not path.exists()
                and not Path(str(path) + ".assets").exists()
                and not Path(str(path) + ".events.jsonl").exists()
                and not marker.exists(),
                "owned sidecars remained",
            )
            assert_true(
                sentinel.read_text() == "project files survive", "project data was modified"
            )
            assert_true(
                not client.json("/api/sessions?refresh=1")[1]["sessions"],
                "deleted session resurrected",
            )
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            assert_true(
                not client.json("/api/sessions?refresh=1")[1]["sessions"],
                "deleted draft resurrected on restart",
            )


def test_web_delete_takes_what_the_conversation_owns(root, home, *, binary):
    """Deleting a conversation removes what sits beside it. An agent it
    started is a conversation of its own, listed under it, and stays."""
    said = [{"role": "user", "content": "hi"}]
    lead = write_session(home, "lead", said, cwd=root)
    agent = write_session(
        home, "agent-0a1b2c3d", said, cwd=root, agent={"parent": fnv1a64(str(lead))}
    )
    (lead.parent / "lead.json.pinned").write_text("{}")
    with Server([event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            sessions = client.json("/api/sessions")[1]["sessions"]
            saved = next(item for item in sessions if item.get("title") == "lead")
            listed = next(item for item in sessions if item.get("title") == "agent-0a1b2c3d")
            assert_true(listed["parent"] == saved["id"], listed)
            client.command("delete", saved)
    left = sorted(path.name for path in lead.parent.iterdir())
    assert_true(not lead.exists(), left)
    assert_true(not any(name.startswith("lead.json.") for name in left), left)
    assert_true(agent.exists(), left)


def test_web_immediate_message_model_control_and_receipts(root, home, *, binary):
    project = root / "message-controls"
    project.mkdir()
    started = threading.Event()
    release = threading.Event()

    def answer(_, _body):
        started.set()
        assert release.wait(timeout=budget(10)), "test did not release provider"
        return event({"content": "Confirmed response"})

    efforts = ["low", "medium", "high", "xhigh", "max"]
    catalog_started = threading.Event()
    release_catalog = threading.Event()

    def catalog_response(_handler):
        catalog_started.set()
        assert release_catalog.wait(timeout=budget(10)), "test did not release catalog"
        return {"data": [{"id": "test", "supported_reasoning_efforts": efforts}]}

    with Server([answer], get_response=catalog_response) as provider:
        providers = {
            "local": {
                "base_url": provider.url,
                "context": 16384,
                "models": {"main": {"id": "test"}},
            }
        }
        with web_host(
            binary,
            root,
            home,
            provider.url,
            extra_env={"UAGENT_PROVIDERS": json.dumps(providers), "UAGENT_MODEL": "local/main"},
        ) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            catalogs = []
            reader = threading.Thread(
                target=lambda: catalogs.append(
                    client.command("model", session, operation="catalog")
                )
            )
            reader.start()
            try:
                assert catalog_started.wait(timeout=budget(5))
                snapshot = client.snapshot(session)
                assert_true(
                    not snapshot["metadata"]["turn_active"] and not snapshot["pending"], snapshot
                )
                # One input can wait behind a control without becoming guidance.
                client.command("submit", session, text="Visible before provider responds")
                request_id = f"{client.sequence:032x}"
                assert_true(not provider.requests, "input ran before the model control finished")
            finally:
                release_catalog.set()
                reader.join(timeout=budget(10))
            assert_true(not reader.is_alive() and catalogs, "catalog receipt did not complete")
            catalog = catalogs[0]
            models = catalog.get("result", {}).get("models", [])
            assert_true(len(models) == 1 and models[0]["value"] == "local/main", catalog)
            assert_true(models[0]["efforts"] == efforts, catalog)
            try:
                assert started.wait(timeout=budget(5))
                snapshot = client.until(
                    session,
                    lambda value: (
                        value["metadata"]["turn_active"]
                        and any(row["kind"] == "turn" for row in value["state"]["view"]["nodes"])
                    ),
                )
                assert_true(snapshot["state"]["efforts"] == ["default", *efforts], snapshot)
                rows = snapshot["state"]["view"]["nodes"]
                users = [row for row in rows if row["kind"] == "turn"]
                assert_true(len(users) == 1 and users[0]["request_id"] == request_id, rows)
                # The reply shows nothing until its first text.
                assert_true(not any(row.get("role") == "assistant" for row in rows), rows)
                saved = session_files(home)
                assert_true(len(saved) == 1 and "Visible before" in saved[0].read_text(), saved)
                receipt = client.json(f"/api/receipts/{request_id}")[1]
                assert_true(receipt["accepted"] and not receipt.get("pending"), receipt)
            finally:
                release.set()
            final = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            assert_true(final["metadata"]["incoming"] == 1, final)
            client.command("close", session)
            catalog = client.json("/api/sessions?refresh=1")[1]
            assert_true(catalog["sessions"][0]["incoming"] == 1, catalog)
            assert_true(len(provider.requests) == 1, "control or inspection invoked the model")


def test_web_control_queues_behind_inflight_catalog(root, home, *, binary):
    catalog_started = threading.Event()
    release = threading.Event()

    def catalog(_handler):
        catalog_started.set()
        assert release.wait(timeout=budget(10)), "catalog was not released"
        return {"data": [{"id": "test"}]}

    with Server([event({"content": "unexpected model call"})], get_response=catalog) as provider:
        providers = {
            "local": {
                "base_url": provider.url,
                "context": 16384,
                "models": {"main": {"id": "test"}},
            }
        }
        with web_host(
            binary,
            root,
            home,
            provider.url,
            extra_env={
                "UAGENT_PROVIDERS": json.dumps(providers),
                "UAGENT_MODEL": "local/main",
            },
        ) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            catalogs = []
            reader = threading.Thread(
                target=lambda: catalogs.append(
                    client.command("model", session, operation="catalog")
                )
            )
            reader.start()
            try:
                assert catalog_started.wait(timeout=budget(5)), "catalog did not start"
                queued = client.command("context", session)
                request_id = f"{client.sequence:032x}"
                assert_true(queued.get("pending"), "control ran before catalog finished")
            finally:
                release.set()
                reader.join(timeout=budget(10))
            assert_true(not reader.is_alive() and catalogs, "catalog did not complete")

            def completed():
                receipt = client.json(f"/api/receipts/{request_id}")[1]
                if receipt.get("pending"):
                    return False
                assert_true(receipt.get("accepted") and "exchanges" in receipt["result"], receipt)
                return True

            wait_until(completed, "queued prompt inspection did not complete")
            assert_true(not provider.requests, "controls invoked the model")


def test_web_background_inspection_and_full_exchange(root, home, *, binary):
    project = root / "background-inspection"
    project.mkdir()
    content = "α🙂" * 5000 + "FULL_BODY_END"
    (project / "large.txt").write_text(content)

    def answer(_, body):
        results = [message for message in body["messages"] if message.get("role") == "tool"]
        if not results:
            return tool_call(
                "run",
                {"command": "sleep 1; printf BACKGROUND_DONE", "yield_ms": 250},
                call_id="background-run",
            )
        if len(results) == 1:
            return tool_call("read_path", {"path": "large.txt"}, call_id="full-read")
        return event({"content": "Foreground done"})

    with Server([answer]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("submit", session, text="/yolo")
            client.until(session, lambda value: value["state"].get("yolo", False))
            client.command("submit", session, text="Run background and read the complete file")
            snapshot = client.until(
                session,
                lambda value: any(
                    row.get("status") == "completed" for row in value["state"].get("activities", [])
                ),
            )
            row = snapshot["state"]["activities"][0]
            first = client.command("activity", session, operation="inspect", activity_id=row["id"])[
                "result"
            ]
            second = client.command(
                "activity", session, operation="inspect", activity_id=row["id"]
            )["result"]
            assert_true(first == second and "BACKGROUND_DONE" in first["output"], (first, second))
            # The command's own row runs until it ends and then takes its
            # result: no receipt row of its own.
            snapshot = client.until(
                session,
                lambda value: any(
                    node["kind"] == "call"
                    and node["state"] == "ok"
                    and "BACKGROUND_DONE" in node.get("body", {}).get("text", "")
                    for node in value["state"]["view"]["nodes"]
                ),
            )
            nodes = snapshot["state"]["view"]["nodes"]
            assert_true(not [node for node in nodes if node["kind"] == "notice"], nodes)
            # The read is the turn's second call, a node by its own id.
            detail_id = [node["id"] for node in nodes if node["kind"] == "call"][1]
            raw = ""
            offset = 0
            pages = 0
            while True:
                status, page, _ = client.json(
                    f"/api/sessions/{session['id']}?detail={detail_id}&raw=1&offset={offset}"
                )
                assert_true(status == 200, page)
                raw += page["text"]
                pages += 1
                if not page["more"]:
                    break
                assert_true(page["next"] > offset, page)
                offset = page["next"]
            # Paged like every long text, retained or still inline.
            assert_true(pages > 1, pages)
            exchange = json.loads(raw)
            assert_true(exchange["complete"] and "FULL_BODY_END" in exchange["response"], exchange)
            assert_true("�" not in exchange["response"], "UTF-8 page boundary split")
            # The command's result reaches the model in the turn or, finished
            # after it, in a turn of its own; inspect once that has ended.
            client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and any(
                        "[completed activities" in json.dumps(body["messages"])
                        for _, body in provider.requests
                    )
                ),
            )
            count = len(provider.requests)
            client.command("activity", session, operation="list")
            assert_true(len(provider.requests) == count, "inspection invoked model")


def test_web_background_command_keeps_its_row_and_waits_are_routine(root, home, *, binary):
    project = root / "background-row"
    project.mkdir()

    def answer(_, body):
        results = [message for message in body["messages"] if message.get("role") == "tool"]
        if not results:
            return tool_call(
                "run", {"command": "sleep 1; printf ROW_DONE", "yield_ms": 250}, call_id="run-1"
            )
        if len(results) == 1:
            return tool_call("activity", {"operation": "wait", "wait_ms": 5000}, call_id="wait-1")
        return event({"content": "Waited"})

    with Server([answer]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("submit", session, text="/yolo")
            client.until(session, lambda value: value["state"].get("yolo", False))
            client.command("submit", session, text="Run it behind and wait")
            snapshot = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and any(
                        node.get("body", {}).get("text") == "Waited"
                        for node in value["state"]["view"]["nodes"]
                    )
                ),
            )
            calls = [node for node in snapshot["state"]["view"]["nodes"] if node["kind"] == "call"]
            run_row, wait_row = calls
            # The wait took the command's end: its result is on the command's
            # own row, and the wait shows only at full detail.
            assert_true(run_row["state"] == "ok" and "ROW_DONE" in run_row["body"]["text"], run_row)
            assert_true(run_row["result"]["exit"] == {"code": 0}, run_row)
            assert_true(wait_row.get("routine") is True and not run_row.get("routine"), calls)


def test_web_stopped_background_command_ends_its_row(root, home, *, binary):
    project = root / "background-stop"
    project.mkdir()

    def answer(_, body):
        if not [message for message in body["messages"] if message.get("role") == "tool"]:
            return tool_call("run", {"command": "sleep 30", "yield_ms": 250}, call_id="run-1")
        return event({"content": "Left running"})

    with Server([answer]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("submit", session, text="/yolo")
            client.until(session, lambda value: value["state"].get("yolo", False))
            client.command("submit", session, text="Run it behind")
            snapshot = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and any(row.get("status") == "running" for row in value["state"]["activities"])
                ),
            )
            activity = snapshot["state"]["activities"][0]["id"]
            client.command("activity", session, operation="stop", activity_id=activity)
            # A stop ends the row the command started on, as its own end would.
            snapshot = client.until(
                session,
                lambda value: any(
                    node["kind"] == "call" and node["state"] == "cancelled"
                    for node in value["state"]["view"]["nodes"]
                ),
            )
            row = next(n for n in snapshot["state"]["view"]["nodes"] if n["kind"] == "call")
            assert_true(row["result"]["word"].startswith("stopped"), row)


def test_web_detail_is_one_level_pushed_to_every_browser(root, home, *, binary):
    import http.client

    with Server([event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            listing = client.json("/api/sessions")[1]
            # The level a browser starts at comes from the host.
            assert_true(listing["detail"] == "default", listing["detail"])
            connection = http.client.HTTPConnection("127.0.0.1", client.port, timeout=15)
            connection.request(
                "GET",
                f"/api/events?cursor={listing['epoch']}:{listing['cursor']}",
                headers={"Cookie": client.cookie},
            )
            response = connection.getresponse()
            assert_true(response.status == 200, response.status)
            pushed = []

            def read_events():
                while line := response.readline():
                    if line.startswith(b"data: "):
                        frame = json.loads(line[6:])
                        if frame.get("kind") == "settings.changed":
                            pushed.append(frame["level"])

            threading.Thread(target=read_events, daemon=True).start()
            client.command(
                "config",
                operation="apply",
                scope="user",
                changes=[{"key": "UAGENT_DETAIL", "value": "minimal"}],
            )
            wait_until(lambda: pushed == ["minimal"], f"change was not pushed: {pushed}")
            # A terminal saves to the same place: browsers follow.
            save_settings(home, binary=binary, UAGENT_DETAIL="full")
            wait_until(lambda: pushed == ["minimal", "full"], f"change not pushed: {pushed}")
            assert_true(client.json("/api/sessions")[1]["detail"] == "full", "stale level")
            # The reader ends with the host; closing here would wait on it.


def test_web_pins_are_one_list_for_every_browser_and_survive_a_restart(root, home, *, binary):
    import http.client

    pins = [{"kind": "folder", "id": str(root)}, {"kind": "session", "id": "a" * 16}]
    with Server([event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, host, _):
            client.pair(code)
            listing = client.json("/api/sessions")[1]
            assert_true(listing["pins"] == [], listing["pins"])
            connection = http.client.HTTPConnection("127.0.0.1", client.port, timeout=15)
            connection.request(
                "GET",
                f"/api/events?cursor={listing['epoch']}:{listing['cursor']}",
                headers={"Cookie": client.cookie},
            )
            response = connection.getresponse()
            assert_true(response.status == 200, response.status)
            pushed = []

            def read_events():
                while line := response.readline():
                    if line.startswith(b"data: "):
                        frame = json.loads(line[6:])
                        if frame.get("kind") == "pins.changed":
                            pushed.append(frame["pins"])

            threading.Thread(target=read_events, daemon=True).start()
            result = client.command("pins", pins=pins)["result"]
            assert_true(result["pins"] == pins, result)
            wait_until(lambda: pushed == [pins], f"pins were not pushed: {pushed}")
            assert_true(client.json("/api/sessions")[1]["pins"] == pins, "pins not listed")
            for number, bad in enumerate(
                (
                    [{"kind": "folder", "id": str(root)}] * 65,
                    [{"kind": "file", "id": "x"}],
                    [{"kind": "session"}],
                    ["x"],
                    {"kind": "folder", "id": "x"},
                )
            ):
                status, refused, _ = client.json(
                    "/api/command",
                    {"v": 2, "kind": "pins", "request_id": f"{0xE0 + number:032x}", "pins": bad},
                )
                assert_true(status == 409, (bad, refused))
            assert_true(client.json("/api/sessions")[1]["pins"] == pins, "a refusal changed pins")
            host.send_signal(signal.SIGTERM)
            host.wait(timeout=10)
            with web_host(binary, root, home, provider.url) as (again, code, _, _):
                again.pair(code)
                kept = again.json("/api/sessions")[1]["pins"]
                assert_true(kept == pins, kept)
