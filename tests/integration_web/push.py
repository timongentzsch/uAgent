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
                block.get("summary", {})
                for block in done["state"]["view"]["blocks"]
                if block.get("summary")
            ]
            assert_true(
                summaries and all(summary.get("outcome") == "complete" for summary in summaries),
                done["state"]["view"]["blocks"],
            )


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
            summaries = [block for block in done["state"]["view"]["blocks"] if block.get("summary")]
            assert_true(len(summaries) == 2, done["state"]["view"]["blocks"])


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
