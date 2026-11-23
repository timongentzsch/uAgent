"""A side chat belongs to its conversation until it is opened as one."""

import json

from integration_support import Server, assert_true, event, wait_until
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


def test_web_side_chats_are_deleted_with_their_conversation(root, home, *, binary):
    with Server([lambda *_: event({"content": "said"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            parent = client.create(root)
            client.command("submit", parent, text="hello")
            client.until(parent, lambda value: idle(value) and "said" in json.dumps(value))
            sides = [started(client, parent, f"request-{n}") for n in range(2)]
            client.command("activate", {"id": sides[0], "generation": ""})
            client.command("close", client.snapshot(parent)["metadata"])
            client.command("delete", client.snapshot(parent)["metadata"])
            left = listing(client)
            assert_true(not {parent["id"], *sides} & left.keys(), left)
            history = home / ".uagent" / "history"
            assert_true(not list(history.rglob("fork-*.json")), list(history.rglob("*")))
