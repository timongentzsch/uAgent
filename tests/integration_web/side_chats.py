"""A side chat belongs to its conversation until it is opened as one."""

import json

from integration_support import Server, assert_true, event, session_id, wait_until, write_session
from web_support import web_host


def idle(value):
    return value["metadata"]["status"] == "idle"


def listing(client):
    return {item["id"]: item for item in client.json("/api/sessions?refresh=1")[1]["sessions"]}


def started(client, parent, request):
    fork = client.command("side", parent, operation="create", text="why?", request=request)
    return fork["result"]["id"]


def test_web_side_chat_belongs_to_its_conversation_until_detached(root, home, *, binary):
    with Server([lambda *_: event({"content": "said"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            parent = client.create(root)
            client.command("submit", parent, text="hello")
            client.until(parent, lambda value: idle(value) and "said" in json.dumps(value))
            side = started(client, parent, "first-request")
            item = listing(client)[side]
            assert_true(item["parent"] == parent["id"], item)
            side_of = item["side"]
            assert_true(side_of["parent"] == parent["id"], item)
            assert_true(side_of["request"] == "first-request", item)
            assert_true(item["title"] == "why?", item)
            session = client.command("activate", {"id": side, "generation": ""})["session"]
            client.until(session, idle)
            client.command("side", client.snapshot(session)["metadata"], operation="detach")

            def own():
                item = listing(client)[side]
                return item["parent"] == "" and item["side"].get("detached")

            wait_until(own, "the side chat stayed one")


def test_web_side_chat_legacy_forks_keep_their_actual_parent(root, home, *, binary):
    messages = [{"role": "user", "content": "earlier question"}]
    lead = write_session(home, "lead", messages, cwd=root)
    side_parent = session_id(lead)
    source = lead
    for name in ("side", "independent"):
        source = write_session(
            home,
            name,
            messages,
            cwd=root,
            parent_session_id=source.stem,
            display={
                "ids": [1],
                "next": 2,
                "facts": {
                    "fork-origin": {"source": session_id(source)},
                    "side-origin": {"parent": side_parent},
                },
            },
        )
    with Server([lambda *_: event({"content": "said"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            rows = listing(client)
            assert_true(
                rows[session_id(lead.with_name("side.json"))]["parent"] == side_parent, rows
            )
            assert_true(
                not rows[session_id(source)]["parent"]
                and rows[session_id(source)].get("side") is None,
                rows,
            )


def test_web_side_chats_are_deleted_with_their_conversation(root, home, *, binary):
    with Server([lambda *_: event({"content": "said"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            parent = client.create(root)
            client.command("submit", parent, text="hello")
            client.until(parent, lambda value: idle(value) and "said" in json.dumps(value))
            sides = [started(client, parent, f"request-{n}") for n in range(2)]
            active = client.command("activate", {"id": sides[0], "generation": ""})["session"]
            independent = client.command("fork", active, title="Independent fork")["result"]
            own = listing(client)[independent["id"]]
            assert_true(not own["parent"] and own.get("side") is None, own)
            client.command("close", client.snapshot(parent)["metadata"])
            client.command("delete", client.snapshot(parent)["metadata"])
            left = listing(client)
            assert_true(not {parent["id"], *sides} & left.keys(), left)
            history = home / ".uagent" / "history"
            assert_true(
                [str(path) for path in history.rglob("fork-*.json")] == [independent["path"]], left
            )


def test_web_merge_reaches_the_conversation_and_keeps_its_title(root, home, *, binary):
    """The summary goes as a report the conversation answers; its first line
    never names the conversation, and the side chat says when it merged."""

    def route(_, body):
        text = json.dumps(body["messages"])
        if "merges this side chat" in text:
            return event({"content": "SIDE-SUMMARY"})
        if "SIDE-SUMMARY" in text:
            return event({"content": "PARENT-READ-IT"})
        return event({"content": "said"})

    with Server([route]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            parent = client.create(root)
            client.command("submit", parent, text="hello")
            client.until(parent, lambda value: idle(value) and "said" in json.dumps(value))
            side = started(client, parent, "first-request")
            session = client.command("activate", {"id": side, "generation": ""})["session"]
            client.until(session, idle)
            client.command("side", client.snapshot(session)["metadata"], operation="merge")
            parent = client.command("activate", {"id": parent["id"], "generation": ""})["session"]
            heard = client.until(parent, lambda value: "PARENT-READ-IT" in json.dumps(value))
            assert_true(heard["metadata"]["title"] == "hello", heard["metadata"])
            merged = client.until(
                session, lambda value: (value["metadata"]["side"] or {}).get("merged")
            )["metadata"]
            assert_true(merged["side"]["merged"]["turns"] == merged["turns"], merged)
            assert_true(not merged["merging"], merged)
            rows = client.snapshot(session)["state"]["view"]["nodes"]
            summed = [node for node in rows if node.get("origin") == "merge"]
            assert_true(
                summed
                and summed[-1]["headline"]["verb"] == "Summed up for"
                and "body" not in summed[-1],
                summed,
            )
