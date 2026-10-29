import base64
import json
import os
import threading
import time

from integration_support import (
    Server,
    assert_true,
    budget,
    event,
    tool_call,
    wait_until,
    write_sse_sequence,
)
from web_support import web_host

from integration_web._common import p256_public_key


def test_web_push_announces_pending_approval(root, home, *, binary):
    capture = root / "push.jsonl"

    def responder(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "done"})
        return tool_call("run", {"command": "echo PUSH_PRIVATE_TEXT"}, call_id="push-run")

    env = {
        "UAGENT_WEB_PUSH_CONTACT": "mailto:test@example.invalid",
        "UAGENT_INTERNAL_PUSH_CAPTURE": str(capture),
    }
    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url, extra_env=env) as (client, code, _, _):
            client.pair(code)
            _, catalogue, _ = client.json("/api/sessions")
            if not catalogue["capabilities"]["push"]:
                return  # built without UAGENT_WEB_PUSH
            status, subscribed, _ = client.json(
                "/api/push/subscriptions",
                {
                    "endpoint": "https://fcm.googleapis.com/send/integration",
                    "keys": {
                        "p256dh": p256_public_key(),
                        "auth": base64.urlsafe_b64encode(os.urandom(16)).rstrip(b"=").decode(),
                    },
                },
            )
            assert_true(status == 200 and subscribed["subscribed"], subscribed)
            session = client.create(root)
            client.command("submit", session, text="Run PUSH_PRIVATE_TEXT")
            client.until(session, lambda value: bool(value.get("pending")))
            wait_until(
                lambda: capture.exists() and capture.read_text().strip(),
                "a pending approval sent no push",
                timeout=5,
            )
            deliveries = [json.loads(line) for line in capture.read_text().splitlines()]
            assert_true(len(deliveries) == 1, deliveries)
            delivery = deliveries[0]
            # Only the encrypted, content-free wake-up leaves the host; the
            # payload's plaintext is pinned by the web_push unit test.
            assert_true(delivery["endpoint"].startswith("https://fcm.googleapis.com/"), delivery)
            assert_true(delivery["vapid"] and delivery["bytes"] > 0, delivery)


def test_web_steer_queues_guidance_and_live_accounting(root, home, *, binary):
    workspace = root / "steer-live"
    workspace.mkdir()
    release = threading.Event()

    def responder(handler, body):
        content = json.dumps(body["messages"])
        if "steer-me" in content:
            release.wait(timeout=budget(10))
            return event({"content": "steered answer"})
        write_sse_sequence(
            handler,
            [
                event({"content": "slow chunk one "}, finish=None),
                event({"content": "slow chunk two "}, finish=None),
                event(
                    {"content": "slow chunk three"},
                    usage={"prompt_tokens": 50, "completion_tokens": 20},
                ),
            ],
            delay=0.3,
        )
        return None

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(workspace)
            client.command("submit", session, text="/yolo")
            client.until(session, lambda value: value["state"].get("yolo", False))
            client.command("submit", session, text="Slow stream probe")
            client.until(session, lambda value: value["metadata"]["turn_active"])
            time.sleep(budget(0.5))
            client.command("steer", session, text="steer-me guidance")
            # The first model call lands while the steered turn still runs:
            # usage and statistics arrive through a live state publish.
            live = client.until(
                session,
                lambda value: (
                    value["metadata"]["turn_active"]
                    and (value["state"].get("statistics") or {}).get("model_calls", 0) >= 1
                ),
            )
            assert_true(not live["state"].get("error"), live["state"])
            release.set()
            done = client.until(session, lambda value: not value["metadata"]["turn_active"])
            assert_true("steered answer" in json.dumps(done), done)
            assert_true(not done["state"].get("error"), done["state"])
            assert_true("interrupted" not in json.dumps(done["state"]), done["state"])
            # The resumed turn records no interrupt detail either.
            assert_true(
                done["state"].get("stop", {}).get("detail", "") == "", done["state"].get("stop")
            )
            summaries = [
                node.get("summary", {})
                for node in done["state"]["view"]["nodes"]
                if node.get("summary")
            ]
            assert_true(
                summaries and all(summary.get("outcome") == "complete" for summary in summaries),
                done["state"]["view"]["nodes"],
            )


def test_web_guidance_starts_its_own_work(root, home, *, binary):
    """Guidance read between steps shows where it was read, after the work
    before it, and the work after it replies to it, live and saved alike."""
    workspace = root / "steer-work"
    workspace.mkdir()
    (workspace / "a.txt").write_text("alpha")
    (workspace / "b.txt").write_text("beta")
    entered, steered = threading.Event(), threading.Event()

    def responder(_, body):
        called = {message.get("tool_call_id") for message in body["messages"]}
        if "first" not in called:
            # The first step holds until the guidance waits behind it.
            entered.set()
            steered.wait(timeout=budget(10))
            return tool_call("read_path", {"path": "a.txt"}, call_id="first")
        if "second" not in called:
            return tool_call("read_path", {"path": "b.txt"}, call_id="second")
        return event({"content": "steered answer"})

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(workspace)
            client.command("submit", session, text="Read a.txt")
            assert_true(entered.wait(budget(10)), "the turn never asked the model")
            client.command("steer", session, text="also read b.txt")
            steered.set()
            done = client.until(
                session,
                lambda value: (
                    not value["metadata"]["turn_active"] and "steered answer" in json.dumps(value)
                ),
            )
            nodes = done["state"]["view"]["nodes"]
            turn = nodes[0]["id"]
            guidance = next(node["id"] for node in nodes if node.get("role") == "user")
            shape = [(node["kind"], node["parent"]) for node in nodes]
            assert_true(
                shape
                == [
                    ("turn", None),
                    ("work", turn),
                    ("call", f"w-{turn}"),
                    ("message", turn),
                    ("work", guidance),
                    ("call", f"w-{guidance}"),
                    ("message", turn),
                    ("footer", turn),
                ],
                nodes,
            )
            status, saved, _ = client.json(f"/api/sessions/{session['id']}?before=0")
            assert_true(status == 200, saved)
            saved_shape = [(node["id"], node["parent"]) for node in saved["state"]["view"]["nodes"]]
            assert_true(saved_shape == [(node["id"], node["parent"]) for node in nodes], saved)


def test_web_a_message_with_files_during_a_turn_is_guidance(root, home, *, binary):
    """Sent while a turn runs, a message with files steers it as a steer
    would, instead of being turned away as busy."""
    workspace = root / "files-steer"
    workspace.mkdir()
    sent_mid_turn = threading.Event()
    guided = []

    def responder(_, body):
        content = json.dumps(body["messages"])
        if "files-guidance" in content:
            guided.append(content)
            return event({"content": "read with the file"})
        # The turn holds until the message is in, so it lands mid-turn.
        sent_mid_turn.wait(timeout=budget(10))
        return event({"content": "first answer"})

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(workspace)
            status, asset, _ = client.json(
                f"/api/sessions/{session['id']}/attachments?name=notes.txt",
                raw=b"attached notes",
                headers={"Content-Type": "text/plain"},
            )
            assert_true(status == 200, asset)
            client.command("submit", session, text="Slow files probe")
            client.until(session, lambda value: value["metadata"]["turn_active"])
            sent = client.command(
                "submit", session, text="files-guidance", attachment_ids=[asset["id"]]
            )
            sent_mid_turn.set()
            assert_true(sent["accepted"] and not sent["error"], sent)
            done = client.until(
                session,
                lambda value: (
                    not value["metadata"]["turn_active"]
                    and "read with the file" in json.dumps(value)
                ),
            )
            assert_true(not done["state"].get("error"), done["state"])
            assert_true(guided and "notes.txt" in guided[-1], guided)


def test_web_queue_next_runs_after_the_turn(root, home, *, binary):
    # "Queue next" never joins the running turn: it waits for the turn to end
    # and then runs as a turn of its own.
    workspace = root / "queue-next"
    workspace.mkdir()

    def responder(handler, body):
        if "queue-me" in json.dumps(body["messages"]):
            return event({"content": "queued answer"})
        write_sse_sequence(
            handler,
            [
                event({"content": "slow chunk one "}, finish=None),
                event({"content": "slow chunk two"}),
            ],
            delay=0.5,
        )
        return None

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(workspace)
            client.command("submit", session, text="Slow queue probe")
            client.until(session, lambda value: value["metadata"]["turn_active"])
            client.command("steer", session, text="queue-me next", queue=True)
            done = client.until(
                session,
                lambda value: (
                    not value["metadata"]["turn_active"] and "queued answer" in json.dumps(value)
                ),
            )
            summaries = [node for node in done["state"]["view"]["nodes"] if node.get("summary")]
            assert_true(len(summaries) == 2, done["state"]["view"]["nodes"])


def test_web_recall_queued_guidance(root, home, *, binary):
    workspace = root / "recall-steer"
    workspace.mkdir()
    release = threading.Event()

    def responder(handler, body):
        content = json.dumps(body["messages"])
        if "recall-me" in content or "deliver-me" in content:
            if "deliver-me" in content:
                return event({"content": "delivered-ok"})
            release.wait(timeout=budget(10))
            return event({"content": "recalled-oops"})
        write_sse_sequence(
            handler,
            [
                event({"content": "slow chunk one "}, finish=None),
                event({"content": "slow chunk two "}, finish=None),
                event(
                    {"content": "slow chunk three"},
                    usage={"prompt_tokens": 50, "completion_tokens": 20},
                ),
            ],
            delay=1.0,
        )
        return None

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(workspace)
            client.command("submit", session, text="Slow recall probe")
            client.until(session, lambda value: value["metadata"]["turn_active"])
            time.sleep(budget(0.5))
            # Still inside the first slow model request: the queue is
            # untouched, so the recall lands deterministically.
            client.command("steer", session, text="recall-me guidance")
            recalled = f"{client.sequence:032x}"
            outcome = client.command("recall", session, target_id=recalled)
            assert_true(outcome.get("accepted"), outcome)
            release.set()
            done = client.until(session, lambda value: not value["metadata"]["turn_active"])
            assert_true("recalled-oops" not in json.dumps(done), done)
            # Undelivered steering stays deliverable through the same path.
            client.command("submit", session, text="Second probe")
            client.until(session, lambda value: value["metadata"]["turn_active"])
            client.command("steer", session, text="deliver-me guidance")
            delivered = f"{client.sequence:032x}"
            done = client.until(
                session,
                lambda value: (
                    not value["metadata"]["turn_active"] and "delivered-ok" in json.dumps(value)
                ),
            )
            assert_true("delivered-ok" in json.dumps(done), done)
            # Consumed steering is never resurrected.
            status, result, _ = client.json(
                "/api/command",
                {
                    "v": 2,
                    "kind": "recall",
                    "request_id": f"{client.sequence + 100:032x}",
                    "session_id": session["id"],
                    "generation": session.get("generation", ""),
                    "target_id": delivered,
                },
            )
            assert_true(status == 409 and "already delivered" in result.get("error", ""), result)


def test_web_insert_queued_guidance(root, home, *, binary):
    workspace = root / "insert-steer"
    workspace.mkdir()
    release = threading.Event()
    started = threading.Event()
    finished = threading.Event()

    def responder(_handler, body):
        if "insert-me" in json.dumps(body["messages"]):
            return event({"content": "inserted-ok"})
        started.set()
        release.wait(timeout=budget(15))
        finished.set()
        return event({"content": "original-finished"})

    try:
        with Server([responder]) as provider:
            with web_host(binary, root, home, provider.url) as (client, code, _, _):
                client.pair(code)
                session = client.create(workspace)
                client.command("submit", session, text="Insert probe")
                assert_true(started.wait(timeout=budget(5)), "provider did not start")
                client.command("steer", session, text="insert-me", queue=True)
                target = f"{client.sequence:032x}"
                outcome = client.command("insert", session, target_id=target)
                assert_true(outcome.get("accepted"), outcome)
                done = client.until(
                    session,
                    lambda value: (
                        not value["metadata"]["turn_active"] and "inserted-ok" in json.dumps(value)
                    ),
                )
                assert_true(not finished.is_set(), "insert waited for original model call")
                users = [
                    b
                    for b in done["state"]["view"]["nodes"]
                    if b.get("kind") == "turn" or b.get("role") == "user"
                ]
                assert_true(sum(b.get("request_id") == target for b in users) == 1, users)
                assert_true(done["state"]["stop"]["reason"] == "completed", done)
                # A stale insert must not start or interrupt another turn.
                status, result, _ = client.json(
                    "/api/command",
                    {
                        "v": 2,
                        "kind": "insert",
                        "request_id": f"{client.sequence + 100:032x}",
                        "session_id": session["id"],
                        "generation": session["generation"],
                        "target_id": target,
                    },
                )
                assert_true(
                    status == 409 and "already delivered" in result.get("error", ""), result
                )
                release.set()
    finally:
        release.set()
