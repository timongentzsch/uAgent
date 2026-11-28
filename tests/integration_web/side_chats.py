"""A side chat belongs to its conversation until it is opened as one."""

import json

from integration_support import Server, assert_true, event, wait_until
from web_support import idle, web_host


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


def test_web_side_chat_is_deleted_alone_and_its_merge_stays(root, home, *, binary):
    def route(_, body):
        text = json.dumps(body["messages"])
        if "merges this side chat" in text:
            return event({"content": "SIDE-SUMMARY"})
        return event({"content": "said"})

    with Server([route]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            parent = client.create(root)
            client.command("submit", parent, text="hello")
            client.until(parent, lambda value: idle(value) and "said" in json.dumps(value))
            gone, kept = (started(client, parent, f"request-{n}") for n in range(2))
            session = client.command("activate", {"id": gone, "generation": ""})["session"]
            client.until(session, idle)
            client.command("side", client.snapshot(session)["metadata"], operation="merge")
            parent = client.command("activate", {"id": parent["id"], "generation": ""})["session"]

            def summary(value):
                nodes = value["state"]["view"]["nodes"]
                return any((node.get("agent") or {}).get("session") == gone for node in nodes)

            client.until(parent, summary)
            merged = client.until(
                session, lambda value: (value["metadata"]["side"] or {}).get("merged")
            )["metadata"]
            client.command("close", merged)
            client.command("delete", listing(client)[gone])
            left = listing(client)
            assert_true(gone not in left and {parent["id"], kept} <= left.keys(), left)
            assert_true(summary(client.snapshot(parent)), "the merged summary stays")
