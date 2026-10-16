import json
import threading
from pathlib import Path

from integration_support import (
    OPENROUTER_SUPPORTS,
    Server,
    assert_true,
    budget,
    event,
    save_settings,
    session_files,
    tool_call,
    tool_calls,
    tool_results,
    user_texts,
    wait_until,
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
            facts = value["state"]["view"]["blocks"]
            assert_true(all(block.get("time") for block in facts), facts)
            answer = next(block for block in facts if block["kind"] == "assistant")
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
                    block
                    for block in client.snapshot(session)["state"]["view"]["blocks"]
                    if block["kind"] == "assistant"
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
                # The provider and host consume separate connections; receiving
                # the request does not mean the host has drained response.started.
                snapshot = client.until(
                    session,
                    lambda value: any(
                        row["kind"] == "assistant" for row in value["state"]["view"]["blocks"]
                    ),
                )
                assert_true(snapshot["state"]["efforts"] == ["default", *efforts], snapshot)
                rows = snapshot["state"]["view"]["blocks"]
                users = [row for row in rows if row["kind"] == "user"]
                attempts = [row for row in rows if row["kind"] == "assistant"]
                assert_true(len(users) == 1 and users[0]["request_id"] == request_id, rows)
                assert_true(
                    len(attempts) == 1
                    and attempts[0]["response_id"]
                    and attempts[0]["content_complete"] is False,
                    rows,
                )
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
                queued = client.command("self_directive", session, action="show")
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
                assert_true(receipt.get("accepted") and "effective" in receipt["result"], receipt)
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
            snapshot = client.until(
                session,
                lambda value: any(
                    block["kind"] == "activity" for block in value["state"]["view"]["blocks"]
                ),
            )
            blocks = snapshot["state"]["view"]["blocks"]
            assert_true(
                len([block for block in blocks if block["kind"] == "activity"]) == 1, blocks
            )
            detail_id = next(
                candidate
                for block in blocks
                for candidate in [
                    block.get("detail_id") if block.get("call_id") == "full-read" else None,
                    *[
                        tool["detail_id"]
                        for tool in block.get("tools", [])
                        if tool.get("call_id") == "full-read"
                    ],
                ]
                if candidate
            )
            raw = ""
            offset = 0
            while True:
                status, page, _ = client.json(
                    f"/api/sessions/{session['id']}?detail={detail_id}&raw=1&offset={offset}"
                )
                assert_true(status == 200, page)
                raw += page["text"]
                if not page["more"]:
                    break
                assert_true(page["next"] > offset, page)
                offset = page["next"]
            exchange = json.loads(raw)
            assert_true(exchange["complete"] and "FULL_BODY_END" in exchange["response"], exchange)
            assert_true("�" not in exchange["response"], "UTF-8 page boundary split")
            count = len(provider.requests)
            client.command("activity", session, operation="list")
            assert_true(len(provider.requests) == count, "inspection invoked model")


def test_web_child_controls_and_conversation_ownership(root, home, *, binary):
    project = root / "child-controls"
    project.mkdir()
    child_started = threading.Event()
    release_child = threading.Event()

    def answer(_, body):
        users = [
            str(message.get("content", ""))
            for message in body["messages"]
            if message.get("role") == "user"
        ]
        if "WEB_CHILD_SEED" in users:
            if "WEB_CHILD_FOLLOWUP" in users:
                assert any(message.get("content") == "Child result" for message in body["messages"])
                assert body["model"] == "mock/new", body
                return event({"content": "Child follow-up result"})
            child_started.set()
            assert release_child.wait(timeout=budget(10))
            return event({"content": "Child result"})
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "Parent ready"})
        return tool_call(
            "subagent", {"prompt": "WEB_CHILD_SEED", "background": True}, call_id="child-spawn"
        )

    with Server([answer]) as provider:
        with web_host(
            binary,
            root,
            home,
            provider.url,
            extra_env={
                "UAGENT_MODEL": "mock/old",
                "UAGENT_INTERNAL_FEATURES": ",".join(OPENROUTER_SUPPORTS),
            },
        ) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            peer = client.create(project)
            client.command("submit", session, text="/yolo")
            client.until(session, lambda value: value["state"].get("yolo", False))
            client.command("submit", session, text="Delegate a child")
            try:
                assert child_started.wait(timeout=budget(5))
                snapshot = client.until(
                    session, lambda value: bool(value["state"].get("activities"))
                )
                child = snapshot["state"]["activities"][0]
                count = len(provider.requests)
                live = client.command(
                    "activity", session, operation="inspect", activity_id=child["id"]
                )
                blocks = live["result"]["conversation"]["blocks"]
                assert_true(
                    any("WEB_CHILD_SEED" in block.get("text", "") for block in blocks), blocks
                )
                assert_true(len(provider.requests) == count, "inspection called the model")
                for operation in ("inspect", "message"):
                    denied = client.json(
                        "/api/command",
                        {
                            "v": 2,
                            "request_id": ("e" if operation == "inspect" else "f") * 32,
                            "kind": "activity",
                            "operation": operation,
                            "text": "must not reach another conversation",
                            "session_id": peer["id"],
                            "generation": peer["generation"],
                            "agent_id": child["agent_id"],
                        },
                    )
                    assert_true(
                        denied[0] == 409 and "another conversation" in denied[1]["error"], denied[1]
                    )
            finally:
                release_child.set()
            # An idle parent takes up its finished child's result as a turn;
            # controls wait until that turn has ended too.
            snapshot = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and any(
                        row.get("status") == "completed"
                        for row in value["state"].get("activities", [])
                    )
                    and any(
                        "[subagent finished" in json.dumps(body["messages"])
                        for _, body in provider.requests
                    )
                ),
            )
            detail = client.command(
                "activity", session, operation="inspect", activity_id=child["id"]
            )["result"]
            assert_true("Child result" in json.dumps(detail["conversation"]), detail)
            assert_true(detail["context_tokens"] > 0, detail)
            assert_true(detail["context_window"] == snapshot["state"]["context_window"], detail)
            client.command(
                "activity",
                session,
                operation="followup",
                agent_id=child["agent_id"],
                text="WEB_CHILD_FOLLOWUP",
                model="mock/new",
            )
            client.until(
                session,
                lambda value: (
                    len(value["state"].get("activities", [])) >= 2
                    and all(row["status"] == "completed" for row in value["state"]["activities"])
                ),
            )
            detail = client.command(
                "activity", session, operation="inspect", agent_id=child["agent_id"]
            )["result"]
            assert_true("Child follow-up result" in json.dumps(detail["conversation"]), detail)


def test_web_lists_the_children_a_turn_waits_on_and_stops_one(root, home, *, binary):
    """Children of one batch the turn waits for run side by side, are listed
    while they run, and one can be stopped alone."""
    project = root / "waited-children"
    project.mkdir()
    started = {name: threading.Event() for name in ("A", "B")}
    release = threading.Event()

    def answer(_, body):
        users = user_texts(body["messages"])
        for name, seen in started.items():
            if f"WAITED_CHILD_{name}" in users:
                seen.set()
                assert release.wait(timeout=budget(20))
                return event({"content": f"Child {name} result"})
        results = tool_results(body["messages"])
        if results:
            return event({"content": "Parent saw: " + " | ".join(results)})
        return tool_calls(
            [(f"waited-{name}", "subagent", {"prompt": f"WAITED_CHILD_{name}"}) for name in "AB"]
        )

    with Server([answer]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("submit", session, text="/yolo")
            client.until(session, lambda value: value["state"].get("yolo", False))
            client.command("submit", session, text="Delegate two")
            try:
                # Both run before either answers: the batch is not taken in turn.
                assert_true(all(seen.wait(timeout=budget(10)) for seen in started.values()), "")
                snapshot = client.until(
                    session,
                    lambda value: (
                        len(
                            [
                                r
                                for r in value["state"].get("activities", [])
                                if r["kind"] == "agent"
                            ]
                        )
                        == 2
                    ),
                )
                rows = snapshot["state"]["activities"]
                assert_true(all(row["status"] == "running" for row in rows), rows)
                first = next(r for r in rows if "WAITED_CHILD_A" in r["label"])
                stopped = client.command(
                    "activity", session, operation="stop", activity_id=first["id"]
                )
                assert_true("error" not in stopped.get("result", {}), stopped)
            finally:
                release.set()
            done = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            assert_true(not done["state"].get("activities"), done["state"])
            final = json.dumps(provider.requests[-1][1]["messages"])
            assert_true("Child B result" in final and "Child A result" not in final, final)


def test_web_verbosity_is_one_level_pushed_to_every_browser(root, home, *, binary):
    import http.client

    with Server([event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            listing = client.json("/api/sessions")[1]
            # The level and what each level shows come from the host: the
            # browser holds no rule of its own.
            assert_true(listing["verbosity"]["level"] == "default", listing["verbosity"])
            assert_true(
                listing["verbosity"]["order"] == ["minimal", "default", "full"],
                listing["verbosity"],
            )
            levels = listing["verbosity"]["levels"]
            assert_true(
                [levels[name]["work"] for name in ("minimal", "default", "full")]
                == ["turn", "groups", "calls"]
                and levels["full"]["open"]
                and not levels["default"]["open"],
                levels,
            )
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
                changes=[{"key": "UAGENT_VERBOSITY", "value": "minimal"}],
            )
            wait_until(lambda: pushed == ["minimal"], f"change was not pushed: {pushed}")
            # A terminal saves to the same place: browsers follow.
            save_settings(home, UAGENT_VERBOSITY="full")
            wait_until(lambda: pushed == ["minimal", "full"], f"change not pushed: {pushed}")
            assert_true(
                client.json("/api/sessions")[1]["verbosity"]["level"] == "full", "stale level"
            )
            # The reader ends with the host; closing here would wait on it.
