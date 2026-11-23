import json
import os
import pathlib
import shutil
import signal
import subprocess
import tempfile
import threading
import time

from integration_support import (
    Server,
    Skipped,
    assert_true,
    budget,
    event,
    timeout_setting,
    tool_call,
    tool_results,
    wait_until,
    write_http_response,
)
from web_support import session_workers, web_host

PARTIAL_USAGE_GATE_SECONDS = 10
PARTIAL_USAGE_REQUEST_SECONDS = 25


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


def test_a_conversation_uses_the_shared_browser(root, home, *, binary):
    offered = []

    def answer(_, body):
        offered.extend(tool["function"]["name"] for tool in body.get("tools", []))
        return tool_call("browser", {"action": "status"})

    with (
        tempfile.TemporaryDirectory(prefix="ua-browser-", dir=os.path.realpath("/tmp")) as data,
        Server([answer, event({"content": "browser-ok"})]) as provider,
        web_host(binary, root, home, provider.url, extra_env={"UAGENT_BROWSER_DATA": data}) as (
            web,
            code,
            _,
            _,
        ),
    ):
        web.pair(code)
        project = root / "browsing"
        project.mkdir()
        session = web.command("create", cwd=str(project))["session"]
        session = web.command("activate", session)["session"]
        web.until(session, lambda value: value["metadata"]["status"] == "idle")
        web.command("submit", session, text="Check browser availability directly")
        web.until(session, lambda value: "browser-ok" in json.dumps(value))
        assert_true("browser" in offered, offered)
        result = next(m for m in provider.requests[-1][1]["messages"] if m.get("role") == "tool")
        assert_true('"parallel_capacity":0' in result["content"].replace(" ", ""), result)
        configured = web.command("browser", action="configure", parallel_capacity=2)["result"]
        assert_true(configured["parallel_capacity"] == 2, configured)
        assert_true(len(configured["instances"]) == 3, configured)
        assert_true(
            web.json("/api/browser/status?instance_id=2")[1]["instance_id"] == "2", configured
        )
        missing, answer, _ = web.json("/api/browser/status?instance_id=9")
        assert_true(missing == 503 and "error" in answer, answer)


def test_a_crashed_tab_is_not_an_opened_page(root, home, *, binary):
    if not (shutil.which("google-chrome-stable") and shutil.which("Xtigervnc")):
        raise Skipped("needs Google Chrome and TigerVNC")
    host = {}

    def renderers():
        rows = subprocess.check_output(["ps", "-axo", "pid=,ppid=,args="], text=True)
        table = [row.split(maxsplit=2) for row in rows.splitlines()]
        found, parents = [], {str(host["pid"])}
        while parents:
            children = [row for row in table if len(row) == 3 and row[1] in parents]
            found += [int(pid) for pid, _, args in children if "--type=renderer" in args]
            parents = {pid for pid, _, _ in children}
        return found

    def crash():
        # What the OOM killer does to a page.
        killed = renderers()
        for pid in killed:
            os.kill(pid, signal.SIGKILL)
        wait_until(lambda: not set(killed) & set(renderers()), "the pages live on")

    def page(handler):
        if handler.path == "/crashing":
            crash()  # while its page is on the way
        write_http_response(handler, b"<title>Kept</title>Kept page", "text/html")

    def browse(action, path="", before=lambda: None):
        def answer(_, _body):
            before()
            url = {"url": host["site"] + path} if path else {}
            return tool_call("browser", {"action": action, **url})

        return answer

    steps = [
        browse("open", "/kept"),
        browse("read", before=crash),
        browse("tabs"),
        browse("open", "/kept"),
        browse("open", "/crashing"),
        event({"content": "browsed"}),
    ]
    with (
        tempfile.TemporaryDirectory(prefix="ua-b-", dir=os.path.realpath("/tmp")) as data,
        Server(steps, page) as provider,
        # Opening a site asks first otherwise.
        web_host(
            binary,
            root,
            home,
            provider.url,
            extra_env={"UAGENT_BROWSER_DATA": data, "UAGENT_APPROVAL": "yolo"},
        ) as (client, code, process, _),
    ):
        host["site"] = provider.url.removesuffix("/v1")
        host["pid"] = process.pid
        client.pair(code)
        client.command("submit", client.create(root), text="browse")
        wait_until(lambda: len(provider.requests) == len(steps), "the turn did not end")
        opened, read, tabs, reopened, cut = tool_results(provider.requests[-1][1]["messages"])
        crashed = "error: the tab's page crashed"
        assert_true(opened.startswith("open done.") and "Kept page" in opened, opened)
        assert_true(read.startswith(crashed), read)
        assert_true('"crashed":true' in tabs, tabs)
        assert_true(reopened.startswith("open done."), reopened)
        # Not "open done" with a page that is gone: the crash, or the
        # navigation it aborted, whichever Chrome tells first.
        assert_true(cut.startswith(crashed) or "net::ERR_ABORTED" in cut, cut)


def test_web_tool_sourced_attachment_marks_origin(root, home, *, binary):
    from integration_support import SMALL_PNG

    project = root / "attachment-origin"
    project.mkdir()
    (project / "shot.png").write_bytes(SMALL_PNG)

    def answer(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            # Hold the turn open so the drained attachment node is observable
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
                    node.get("origin") == "tool" for node in value["state"]["view"]["nodes"]
                ),
            )
            nodes = snapshot["state"]["view"]["nodes"]
            sourced = [node for node in nodes if node.get("origin") == "tool"]
            assert_true(len(sourced) == 1, nodes)
            # The files a call read sit under that call.
            call = next(node for node in nodes if node["kind"] == "call")
            assert_true(sourced[0]["parent"] == call["id"], sourced)
            # The drained image still reaches the model once.
            client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and any(
                        "seen" in str(node.get("body", {}).get("text", ""))
                        for node in value["state"]["view"]["nodes"]
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
                            node.get("body", {}).get("text") == "second client guidance"
                            for node in value["state"]["view"]["nodes"]
                        )
                    ),
                )
                users = [
                    node["body"]["text"]
                    for node in completed["state"]["view"]["nodes"]
                    if node["kind"] == "turn" or node.get("role") == "user"
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
        # The stream pauses twice while the client observes intermediate
        # state: its transport deadline must exceed both scaled gates.
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
            connection, response = client.events()
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
                    if frame.get("kind") == "node":
                        # The reply's first text comes whole, the rest as
                        # appends.
                        node = frame.get("node") or {}
                        if node.get("role") == "assistant" and "body" in node and not deltas:
                            deltas.append(node["body"]["text"])
                        elif frame.get("append", {}).get("text"):
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
                    node["body"]["text"]
                    for node in reversed(snapshot["state"]["view"]["nodes"])
                    if node.get("role") == "assistant"
                )
                assert_true(answer == "AB" + "x" * burst + "Z", len(answer))
                assert_true(len(deltas) < burst // 4, len(deltas))
            finally:
                release_final.set()
                connection.close()


def test_checkpoints_reach_browsers_as_changed_rows(root, home, *, binary):
    """After the view a browser loaded, a checkpoint sends only the changed
    rows; applying them in order gives the view a fresh load returns."""

    with Server([lambda _, _body: event({"content": "row " * 500})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            client.command("submit", session, text="first")
            loaded = client.until(
                session,
                lambda value: (
                    value["state"].get("turns") == 1 and value["metadata"]["status"] == "idle"
                ),
            )
            connection, response = client.events(loaded)
            frames = []
            seen = [0]

            def read_events():
                while line := response.readline():
                    if not line.startswith(b"data: "):
                        continue
                    frame = json.loads(line[6:])
                    seen[0] = frame.get("sequence", seen[0])
                    if frame.get("session_id") == session["id"]:
                        frames.append(frame)

            reader = threading.Thread(target=read_events, daemon=True)
            reader.start()
            try:
                for text in ("second", "third"):
                    client.command("submit", session, text=text)
                    final = client.until(
                        session,
                        lambda value, text=text: (
                            text in json.dumps(value) and value["metadata"]["status"] == "idle"
                        ),
                    )
                wait_until(lambda: seen[0] >= final["cursor"], "events stopped short")
            finally:
                connection.close()
            checkpoints = [
                frame for frame in frames if frame["kind"] == "state" and frame.get("checkpoint")
            ]
            assert_true(len(checkpoints) >= 2, [frame["kind"] for frame in frames])
            assert_true(all("view" not in frame["state"] for frame in checkpoints), checkpoints)
            nodes = loaded["state"]["view"]["nodes"]
            for frame in frames:
                if frame["kind"] != "node" or frame["sequence"] > final["cursor"]:
                    continue
                row = frame.get("node")
                key = row["id"] if row else frame["id"]
                at = max((i for i, b in enumerate(nodes) if b.get("id") == key), default=-1)
                if row and at >= 0:
                    nodes[at] = row
                elif row:
                    # Before the first node under it, else in sequence position.
                    later = [
                        i
                        for i, b in enumerate(nodes)
                        if b.get("parent") == row["id"]
                        or (
                            isinstance(b.get("sequence"), int)
                            and isinstance(row.get("sequence"), int)
                            and b["sequence"] > row["sequence"]
                        )
                    ]
                    nodes.insert(later[0] if later else len(nodes), row)
                elif at >= 0:
                    nodes[at] = {**nodes[at], **frame.get("set", {})}
                    if "append" in frame:
                        body = dict(nodes[at]["body"])
                        body["text"] += frame["append"]["text"]
                        nodes[at]["body"] = body
            assert_true(nodes == final["state"]["view"]["nodes"], (nodes, final["state"]))


def test_web_compact_renders_single_footer_without_duplicates(root, home, *, binary):
    project = root / "compact-footer"
    project.mkdir()

    def answer(_, body):
        text = "\n".join(str(message.get("content", "")) for message in body["messages"])
        if "for a fresh agent context" in text:
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
                        "turn-ok" in str(node.get("body", {}).get("text", ""))
                        for node in value["state"]["view"]["nodes"]
                    )
                ),
            )
            client.command("submit", session, text="second COMPACT-MARKER")
            client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and sum(
                        "COMPACT-MARKER" in str(node.get("body", {}).get("text", ""))
                        for node in value["state"]["view"]["nodes"]
                    )
                    == 2
                ),
            )
            client.command("submit", session, text="/compact")
            snapshot = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and any("compaction" in node for node in value["state"]["view"]["nodes"])
                ),
            )
            nodes = snapshot["state"]["view"]["nodes"]
            footers = [node for node in nodes if "compaction" in node]
            assert_true(len(footers) == 1, nodes)
            assert_true(
                footers[0]["compaction"]["messages_after"]
                < footers[0]["compaction"]["messages_before"],
                footers,
            )
            markers = [
                node
                for node in nodes
                if "COMPACT-MARKER" in str(node.get("body", {}).get("text", ""))
            ]
            assert_true(len(markers) == 2, nodes)
            assert_true(len({node["id"] for node in nodes}) == len(nodes), nodes)

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
            assert len([b for b in unchanged["state"]["view"]["nodes"] if "compaction" in b]) == 1


def test_looks_fold_into_a_group_and_the_work_counts_by_kind(root, home, *, binary):
    """Two reads in a row are one "Explored" group; what the agent says ends
    the run, so the read after it stands alone, and the work's line counts
    what was done by kind."""
    project = root / "kinds"
    project.mkdir()
    for name in ("a.txt", "b.txt", "c.txt"):
        (project / name).write_text(name)

    def read(name):
        return {
            "index": ord(name[0]) - ord("a"),
            "id": f"read-{name[0]}",
            "function": {"name": "read_path", "arguments": json.dumps({"path": name})},
        }

    def answer(_, body):
        done = {m.get("tool_call_id") for m in body["messages"] if m.get("role") == "tool"}
        if "read-a" not in done:
            return event({"tool_calls": [read("a.txt"), read("b.txt")]})
        if "read-c" not in done:
            return event({"content": "Found it.", "tool_calls": [read("c.txt")]})
        return event({"content": "Done."})

    with Server([answer]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            client.command("submit", session, text="look around")
            value = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            nodes = value["state"]["view"]["nodes"]
            by_kind = lambda kind: [node for node in nodes if node["kind"] == kind]  # noqa: E731
            (work,) = by_kind("work")
            assert_true(work["headline"] == {"verb": "Worked", "target": "3 reads, 1 note"}, work)
            (group,) = by_kind("group")
            assert_true(
                group["headline"] == {"verb": "Explored", "target": "2 files"}
                and group["state"] == "ok"
                and group["parent"] == work["id"],
                group,
            )
            parents = [node["parent"] for node in by_kind("call")]
            assert_true(parents == [group["id"], group["id"], work["id"]], parents)
            # A page read back from the saved conversation is the same tree.
            status, saved, _ = client.json(f"/api/sessions/{session['id']}/view")
            if status == 200:
                kinds = [node["kind"] for node in saved.get("nodes", [])]
                assert_true(kinds.count("group") == 1, kinds)


def test_a_conversation_runtime_goes_before_the_host_when_memory_runs_out(root, home, *, binary):
    if not pathlib.Path("/proc/self/oom_score_adj").exists():
        raise Skipped("the OOM score is Linux's")
    with Server([lambda _, _body: event({"content": "ok"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, process, _):
            client.pair(code)
            client.create(root)

            def score(pid):
                return int(pathlib.Path(f"/proc/{pid}/oom_score_adj").read_text())

            workers = session_workers(process.pid)
            assert_true(workers, "no runtime started")
            assert_true(all(score(pid) > score(process.pid) for pid in workers), workers)


def test_a_message_restarts_a_conversation_whose_runtime_died(root, home, *, binary):
    asked, release = threading.Event(), threading.Event()

    def held(_, _body):
        asked.set()
        release.wait(timeout=budget(10))
        return event({"content": "never seen"})

    with Server([held, event({"content": "answered again"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, process, _):
            client.pair(code)
            session = client.create(root)
            client.command("submit", session, text="a turn the runtime dies in")
            assert_true(asked.wait(timeout=budget(10)), "the model was not asked")
            (worker,) = session_workers(process.pid)
            os.kill(worker, signal.SIGKILL)
            value = client.until(
                session, lambda value: value["metadata"]["status"] == "interrupted"
            )
            release.set()
            assert_true(value["state"]["stop"]["reason"] == "cancelled", value["state"])
            # The generation the client holds names the runtime that died.
            assert_true(value["metadata"]["generation"] == session["generation"], value)
            client.command("submit", session, text="go on")
            client.until(session, lambda value: "answered again" in json.dumps(value["state"]))


def test_a_message_with_the_generation_of_a_runtime_stopped_when_idle_is_answered(
    root, home, *, binary
):
    with Server([event({"content": "answered after idle"})]) as provider:
        with web_host(
            binary, root, home, provider.url, extra_env={"UAGENT_INTERNAL_IDLE_S": "1"}
        ) as (client, code, process, _):
            client.pair(code)
            session = client.create(root)
            wait_until(lambda: not session_workers(process.pid), "the runtime did not stop")
            client.command("submit", session, text="after a while")
            client.until(session, lambda value: "answered after idle" in json.dumps(value["state"]))
