import json
import os
import signal
import subprocess
import tempfile
import threading
import time

from integration_support import (
    Server,
    assert_true,
    budget,
    event,
    timeout_setting,
    tool_call,
    wait_until,
)
from web_support import web_host

from integration_web._common import PARTIAL_USAGE_GATE_SECONDS, PARTIAL_USAGE_REQUEST_SECONDS


def test_browser_appliance_auth_and_handover_guards(root, home, *, binary):
    offered = []

    def answer(_, body):
        offered.extend(tool["function"]["name"] for tool in body.get("tools", []))
        return event({"content": "browser tool offered"})

    with (
        tempfile.TemporaryDirectory(prefix="ua-b-", dir=os.path.realpath("/tmp")) as browser_data,
        Server([answer]) as provider,
    ):
        with web_host(
            binary,
            root,
            home,
            provider.url,
            extra_env={"UAGENT_BROWSER_DATA": browser_data},
        ) as (client, code, _, _):
            assert_true(client.json("/api/browser/status")[0] == 401, "unpaired browser status")
            client.pair(code)
            status, idle, _ = client.json("/api/browser/status")
            assert_true(status == 200 and idle["mode"] == "idle", idle)
            assert_true(idle["running"] is False, idle)
            assert_true(idle["controller"] is False, idle)
            assert_true("viewer" not in idle, idle)
            project = root / "project"
            project.mkdir()
            session = client.create(project)
            client.command("submit", session, text="Check available tools")
            client.until(session, lambda value: "browser tool offered" in json.dumps(value))
            assert_true("browser" in offered, offered)
            client.sequence += 1
            rejected, outcome, _ = client.json(
                "/api/command",
                {
                    "v": 2,
                    "kind": "browser",
                    "request_id": f"{client.sequence:032x}",
                    "action": "done",
                    "interaction_id": "a" * 32,
                },
            )
            assert_true(rejected == 409 and outcome["accepted"] is False, outcome)
            assert_true(
                client.command("browser", action="stop")["result"]["mode"] == "idle", "stop"
            )


def test_panel_moderator_can_use_the_shared_browser(root, home, *, binary):
    offered = []

    def answer(_, body):
        offered.extend(tool["function"]["name"] for tool in body.get("tools", []))
        return tool_call("browser", {"action": "status"})

    with (
        tempfile.TemporaryDirectory(prefix="ua-panel-b-", dir=os.path.realpath("/tmp")) as data,
        Server([answer, event({"content": "moderator-browser-ok"})]) as provider,
        web_host(binary, root, home, provider.url, extra_env={"UAGENT_BROWSER_DATA": data}) as (
            web,
            code,
            _,
            _,
        ),
    ):
        web.pair(code)
        project = root / "moderator"
        project.mkdir()
        session = web.command("create", cwd=str(project), panel=True)["session"]
        session = web.command("activate", session)["session"]
        web.until(session, lambda value: value["metadata"]["status"] == "idle")
        web.command("submit", session, text="Check browser availability directly")
        web.until(session, lambda value: "moderator-browser-ok" in json.dumps(value))
        assert_true("browser" in offered, offered)
        assert_true("run" not in offered and "write_file" not in offered, offered)
        result = next(m for m in provider.requests[-1][1]["messages"] if m.get("role") == "tool")
        assert_true('"parallel_capacity":0' in result["content"].replace(" ", ""), result)
        configured = web.command("browser", action="configure", parallel_capacity=2)["result"]
        assert_true(configured["parallel_capacity"] == 2, configured)
        assert_true(len(configured["instances"]) == 3, configured)
        assert_true(web.json("/api/browser/status?instance=2")[1]["instance_id"] == "2", configured)


def test_web_tool_sourced_attachment_marks_origin(root, home, *, binary):
    from integration_support import SMALL_PNG

    project = root / "attachment-origin"
    project.mkdir()
    (project / "shot.png").write_bytes(SMALL_PNG)

    def answer(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            # Hold the turn open so the drained attachment block is observable
            # mid-turn; the turn-boundary strip reclassifies it afterwards.
            time.sleep(1.0)
            return event({"content": "seen"})
        return tool_call("read_path", {"path": "shot.png"}, call_id="shot-read")

    with Server([answer]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("submit", session, text="/yolo")
            client.until(session, lambda value: value["state"].get("yolo", False))
            client.command("submit", session, text="look at the screenshot")
            snapshot = client.until(
                session,
                lambda value: any(
                    block.get("kind") == "attachment" for block in value["state"]["view"]["blocks"]
                ),
            )
            blocks = snapshot["state"]["view"]["blocks"]
            sourced = [block for block in blocks if block.get("kind") == "attachment"]
            assert_true(len(sourced) == 1, blocks)
            assert_true(sourced[0].get("origin") == "tool", sourced)
            assert_true(sourced[0].get("source_call_ids") == ["shot-read"], sourced)
            # The drained image still reaches the model once.
            client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and any(
                        "seen" in str(block.get("text", ""))
                        for block in value["state"]["view"]["blocks"]
                    )
                ),
            )


def test_terminal_and_web_share_runtime_across_client_restarts(root, home, *, binary):
    from integration_support import run_dialog

    project = root / "shared-clients"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "shared response"})]) as provider:
        with web_host(binary, root, home, provider.url) as (web, code, host, env):
            web.pair(code)
            with (root / "terminal.log").open("w+") as output:
                terminal = subprocess.Popen(
                    [str(binary), "--yolo"],
                    cwd=project,
                    env=env,
                    stdin=subprocess.PIPE,
                    stdout=output,
                    stderr=output,
                    text=True,
                )
                try:
                    terminal.stdin.write("from terminal\n")
                    terminal.stdin.flush()
                    found = {}

                    def discovered():
                        for item in web.json("/api/sessions")[1]["sessions"]:
                            if os.path.realpath(item["cwd"]) == os.path.realpath(project):
                                found.update(item)
                                return True
                        return False

                    wait_until(discovered, "terminal session not discovered", timeout=10)
                    first = web.until(
                        found,
                        lambda value: (
                            value["metadata"]["status"] == "idle"
                            and "shared response" in json.dumps(value)
                        ),
                    )
                    generation = first["metadata"]["generation"]
                    web.command("submit", first["metadata"], text="from browser")
                    web.until(
                        found,
                        lambda value: (
                            value["state"].get("turns") == 2
                            and value["metadata"]["status"] == "idle"
                        ),
                    )
                    terminal.kill()
                    terminal.wait(timeout=5)
                    resumed = run_dialog(
                        project, env, "from second terminal\n/q\n", "-c", binary=binary
                    )
                    assert resumed.returncode == 0, resumed.stderr
                    assert "from browser" in resumed.stdout, resumed.stdout
                    assert "from second terminal" in resumed.stdout, resumed.stdout
                    final = web.until(
                        found,
                        lambda value: (
                            value["state"].get("turns") == 3
                            and value["metadata"]["status"] == "idle"
                        ),
                    )
                    assert final["metadata"]["generation"] == generation
                    assert final["metadata"]["presence"] == "active"
                    host.send_signal(signal.SIGTERM)
                    host.wait(timeout=10)
                    with web_host(binary, root, home, provider.url) as (reconnected, code, _, _):
                        reconnected.pair(code)
                        reconnected.json("/api/sessions")
                        replay = reconnected.until(
                            found,
                            lambda value: (
                                value["metadata"]["presence"] == "active"
                                and "view" in value["state"]
                            ),
                        )
                        assert replay["metadata"]["generation"] == generation
                        assert_true(
                            replay["state"]["view"] == final["state"]["view"],
                            (replay["state"]["view"], final["state"]["view"]),
                        )
                    assert len(provider.requests) == 3, provider.requests
                finally:
                    if terminal.poll() is None:
                        terminal.kill()
                        terminal.wait(timeout=5)
                    terminal.stdin.close()


def test_clients_share_one_runtime_and_reconnect(root, home, *, binary):
    from session_support import SessionClient, runtime_directory

    project = root / "shared-session"
    project.mkdir()
    model_entered = threading.Event()
    model_continue = threading.Event()

    def answer(_, body):
        if not any("second client guidance" in str(m.get("content", "")) for m in body["messages"]):
            model_entered.set()
            assert model_continue.wait(budget(10))
        return event({"content": "shared-answer"})

    with Server([answer]) as provider:
        with web_host(binary, root, home, provider.url) as (web, code, _, env):
            web.pair(code)
            session = web.create(project)
            web.command("submit", session, text="/yolo")
            web.until(session, lambda value: value["state"].get("yolo"))
            path = next(runtime_directory(home).glob("*.sock"))
            first, second = SessionClient(path), SessionClient(path)
            try:

                def ready(frame):
                    return frame.get("kind") == "state" and not frame.get("busy")

                first.until(ready)
                second.until(ready)
                command = first.send("submit", text="first client")
                receipt = first.until(
                    lambda frame: (
                        frame.get("kind") == "outcome"
                        and frame.get("request_id") == command["request_id"]
                    )
                )
                assert receipt.get("accepted"), receipt
                assert model_entered.wait(budget(10)), web.snapshot(session)
                # Same command ID is delivery retry, not another user turn.
                first.socket.sendall((json.dumps(command) + "\n").encode())
                second.send("submit", text="second client guidance")
                second.until(lambda frame: frame.get("kind") == "outcome" and frame.get("accepted"))
                model_continue.set()
                completed = web.until(
                    session,
                    lambda value: (
                        value["metadata"]["status"] == "idle"
                        and any(
                            block.get("text") == "second client guidance"
                            for block in value["state"]["view"]["blocks"]
                        )
                    ),
                )
                users = [
                    block["text"]
                    for block in completed["state"]["view"]["blocks"]
                    if block["kind"] == "user"
                ]
                assert users == ["first client", "second client guidance"], users
                assert completed["state"]["stop"]["reason"] == "completed", completed
                second.close()
                second = SessionClient(path)
                replay = second.until(ready)
                assert_true(
                    replay["state"]["view"] == completed["state"]["view"],
                    (replay["state"]["view"], completed["state"]["view"]),
                )
                assert replay["generation"] == first.hello["generation"]
            finally:
                model_continue.set()
                first.close()
                second.close()


def test_partial_usage_reconciles_without_double_counting(root, home, *, binary):
    release = threading.Event()
    finish = threading.Event()

    def stream(handler, _):
        handler.send_response(200)
        handler.send_header("Content-Type", "text/event-stream")
        handler.end_headers()
        first = event(
            {"content": "Partial answer"},
            finish=None,
            usage={"prompt_tokens": 100, "completion_tokens": 10, "cost": 0.01},
        )
        handler.wfile.write(("data: " + json.dumps(first) + "\n\n").encode())
        handler.wfile.flush()
        assert release.wait(budget(PARTIAL_USAGE_GATE_SECONDS))
        # Context must keep growing even without a new billing sample.
        chunk = event({"content": "x" * 4096}, finish=None)
        handler.wfile.write(("data: " + json.dumps(chunk) + "\n\n").encode())
        handler.wfile.flush()
        assert finish.wait(budget(PARTIAL_USAGE_GATE_SECONDS))
        final = event(
            {"content": " complete"},
            usage={"prompt_tokens": 100, "completion_tokens": 20, "cost": 0.03},
        )
        handler.wfile.write(("data: " + json.dumps(final) + "\n\ndata: [DONE]\n\n").encode())
        handler.wfile.flush()

    with Server([stream]) as provider:
        # This stream deliberately pauses twice while the client observes
        # intermediate state. Its transport deadline must exceed both scaled
        # gates under coverage and thread instrumentation.
        with web_host(
            binary,
            root,
            home,
            provider.url,
            extra_env={
                "UAGENT_REQUEST_TIMEOUT": timeout_setting(PARTIAL_USAGE_REQUEST_SECONDS),
                "UAGENT_STREAM_TIMEOUT": timeout_setting(PARTIAL_USAGE_REQUEST_SECONDS),
            },
        ) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            initial = client.snapshot(session)["state"].get("context_tokens", 0)
            try:
                client.command("submit", session, text="Report usage " + "x" * 4096)
                live = client.until(
                    session, lambda value: value["state"].get("usage", {}).get("cost") == 0.01
                )
                assert live["metadata"]["turn_active"], live
                assert abs(time.time() * 1000 - live["state"]["turn_started_ms"]) < 60_000, live
                assert live["state"]["context_tokens"] > initial, live
                release.set()
                growing = client.until(
                    session,
                    lambda value: (
                        value["state"].get("context_tokens", 0)
                        > live["state"]["context_tokens"] + 500
                    ),
                )
                assert growing["metadata"]["turn_active"], growing
                assert growing["state"]["usage"]["cost"] == 0.01, growing
                finish.set()
                done = client.until(session, lambda value: not value["metadata"]["turn_active"])
                assert done["state"]["usage"]["cost"] == 0.03, done
                assert done["state"]["usage"]["output"] == 20, done
                assert done["state"]["statistics"]["model_calls"] == 1, done
            finally:
                release.set()
                finish.set()


def test_worker_batches_fast_deltas_without_delaying_a_quiet_tail(root, home, *, binary):
    import http.client

    second_written = threading.Event()
    release_final = threading.Event()
    burst = 512

    def stream_response(handler, _body):
        handler.send_response(200)
        handler.send_header("Content-Type", "text/event-stream")
        handler.send_header("Connection", "close")
        handler.end_headers()
        handler.close_connection = True
        for text in ("A", "B"):
            handler.wfile.write(
                ("data: " + json.dumps(event({"content": text}, finish=None)) + "\n\n").encode()
            )
            handler.wfile.flush()
        second_written.set()
        assert release_final.wait(budget(5)), "quiet tail was never observed"
        for _ in range(burst):
            handler.wfile.write(
                ("data: " + json.dumps(event({"content": "x"}, finish=None)) + "\n\n").encode()
            )
        handler.wfile.write(
            (
                "data: "
                + json.dumps(event({"content": "Z"}, usage={"completion_tokens": burst + 3}))
                + "\n\ndata: [DONE]\n\n"
            ).encode()
        )
        handler.wfile.flush()

    with Server([stream_response]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            listing = client.json("/api/sessions")[1]
            connection = http.client.HTTPConnection("127.0.0.1", client.port, timeout=15)
            connection.request(
                "GET",
                f"/api/events?cursor={listing['epoch']}:{listing['cursor']}",
                headers={"Cookie": client.cookie},
            )
            response = connection.getresponse()
            assert_true(response.status == 200, response.status)
            deltas = []
            observed_tail = threading.Event()
            completed = threading.Event()

            def read_events():
                started = False
                while line := response.readline():
                    if not line.startswith(b"data: "):
                        continue
                    frame = json.loads(line[6:])
                    if frame.get("session_id") != session["id"]:
                        continue
                    if frame.get("kind") == "state" and frame.get("busy"):
                        started = True
                    if frame.get("kind") == "event" and frame.get("type") == "turn.started":
                        started = True
                    if frame.get("kind") == "block" and frame.get("append", {}).get("text"):
                        deltas.append(frame["append"]["text"])
                        if "".join(deltas).startswith("AB"):
                            observed_tail.set()
                    if started and frame.get("kind") == "state" and not frame.get("busy"):
                        completed.set()
                        return

            reader = threading.Thread(target=read_events, daemon=True)
            reader.start()
            try:
                client.command("submit", session, text="Batch this stream")
                assert_true(second_written.wait(budget(2)), "provider did not write quiet tail")
                assert_true(
                    observed_tail.wait(budget(2)),
                    "a pending delta was not published at the batching deadline",
                )
                release_final.set()
                assert_true(completed.wait(budget(10)), "turn did not publish its final checkpoint")
                reader.join(timeout=budget(2))
                snapshot = client.snapshot(session)
                answer = next(
                    block["text"]
                    for block in reversed(snapshot["state"]["view"]["blocks"])
                    if block["kind"] == "assistant"
                )
                assert_true(answer == "AB" + "x" * burst + "Z", len(answer))
                assert_true(len(deltas) < burst // 4, len(deltas))
            finally:
                release_final.set()
                connection.close()


def test_web_compact_renders_single_footer_without_duplicates(root, home, *, binary):
    project = root / "compact-footer"
    project.mkdir()

    def answer(_, body):
        text = "\n".join(str(message.get("content", "")) for message in body["messages"])
        if "Summarize the bounded transcript" in text:
            return event({"content": "COMPACT-SUMMARY"})
        return event({"content": "turn-ok " + "detail " * 200})

    with Server([answer]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("submit", session, text="/yolo")
            client.until(session, lambda value: value["state"].get("yolo", False))
            client.command("submit", session, text="first COMPACT-MARKER")
            client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and any(
                        "turn-ok" in str(block.get("text", ""))
                        for block in value["state"]["view"]["blocks"]
                    )
                ),
            )
            client.command("submit", session, text="second COMPACT-MARKER")
            client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and sum(
                        "COMPACT-MARKER" in str(block.get("text", ""))
                        for block in value["state"]["view"]["blocks"]
                    )
                    == 2
                ),
            )
            client.command("submit", session, text="/compact")
            snapshot = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and any(
                        block.get("kind") == "compaction"
                        for block in value["state"]["view"]["blocks"]
                    )
                ),
            )
            blocks = snapshot["state"]["view"]["blocks"]
            footers = [block for block in blocks if block.get("kind") == "compaction"]
            assert_true(len(footers) == 1, blocks)
            assert_true(
                footers[0]["compaction"]["messages_after"]
                < footers[0]["compaction"]["messages_before"],
                footers,
            )
            markers = [block for block in blocks if "COMPACT-MARKER" in str(block.get("text", ""))]
            assert_true(len(markers) == 2, blocks)
            assert_true(len({block["id"] for block in blocks}) == len(blocks), blocks)

            preview = client.command("context", session)["result"]["exchanges"][0]
            assert_true(preview["preview"], preview)
            # Re-compacting this already minimal context must preserve it.
            before = snapshot["state"]["context_tokens"]
            client.command("submit", session, text="/compact")
            unchanged = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and value["state"].get("statistics", {}).get("model_calls", 0) >= 4
                ),
            )
            assert unchanged["state"]["context_tokens"] == before
            assert (
                len([b for b in unchanged["state"]["view"]["blocks"] if b["kind"] == "compaction"])
                == 1
            )
