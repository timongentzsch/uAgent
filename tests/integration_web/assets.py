import json
import threading
import time
from pathlib import Path

from integration_support import (
    Server,
    assert_true,
    budget,
    event,
    session_files,
    tool_call,
    tool_calls,
    tool_results,
    wait_until,
)
from web_support import WebClient, web_host

from integration_web._common import PNG


def test_web_atomic_images_and_session_isolation(root, home, *, binary):
    project = root / "images"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "Image received"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            other = client.create(project)
            client.until(other, lambda value: value["metadata"]["status"] == "idle")
            endpoint = f"/api/sessions/{session['id']}/attachments"
            status, generic, _ = client.json(
                endpoint,
                raw=b"<svg onload='alert(1)'></svg>",
                headers={"Content-Type": "image/png"},
            )
            # SVG sniffs as an image (providers only ever see its raster);
            # the claimed Content-Type is distrusted either way.
            assert_true(
                status == 200 and generic["image"] and generic["mime"] == "image/svg+xml",
                generic,
            )
            status, body, headers = client.request(
                f"/api/sessions/{session['id']}/assets/{generic['id']}"
            )
            # Vectors serve under their sniffed mime (re-verified at read
            # time); script execution is contained by the asset CSP.
            assert_true(
                body == b"<svg onload='alert(1)'></svg>"
                and headers["Content-Type"] == "image/svg+xml",
                headers,
            )
            status, image, _ = client.json(endpoint, raw=PNG, headers={"Content-Type": "image/png"})
            assert_true(status == 200, image)
            status, rejected, _ = client.json(
                "/api/command",
                {
                    "v": 2,
                    "request_id": "a" * 32,
                    "kind": "submit",
                    "session_id": session["id"],
                    "generation": session["generation"],
                    "attachment_ids": [image["id"], "f" * 32],
                    "text": "Reject the incomplete attachment batch",
                },
            )
            assert_true(status == 409 and "unavailable" in rejected["error"], rejected)
            record = json.loads((home / f".uagent/web/drafts/{session['id']}.json").read_text())
            metadata = Path(record["path"] + f".assets/{image['id']}.json")
            assert_true(not json.loads(metadata.read_text())["committed"], metadata.read_text())
            client.command("submit", session, text="", attachment_ids=[image["id"]])
            value = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and bool(value["state"].get("view", {}).get("blocks"))
                ),
            )
            assert_true(image["id"] in json.dumps(value), value)
            assert_true("base64," not in json.dumps(value), value)
            client.command("close", session)
            assert_true(image["id"] in json.dumps(client.snapshot(session)), "image reference lost")
            asset = f"/api/sessions/{session['id']}/assets/{image['id']}"
            assert_true(client.request(asset)[1] == PNG, "source asset changed")
            anonymous = WebClient(client.port)
            assert_true(anonymous.request(asset)[0] == 401, "private image was public")
            outside = root / "separate-workspace"
            outside.mkdir()
            peer = client.create(outside)
            status, denied, _ = client.json(
                "/api/command",
                {
                    "v": 2,
                    "request_id": "b" * 32,
                    "kind": "submit",
                    "session_id": peer["id"],
                    "generation": peer["generation"],
                    "attachment_ids": [image["id"]],
                    "text": "Wrong session image",
                },
            )
            assert_true(status == 409 and "unavailable" in denied["error"], denied)
            record = json.loads((home / f".uagent/web/drafts/{session['id']}.json").read_text())
            source = Path(record["path"] + f".assets/{image['id']}.data")
            source.unlink()
            source.symlink_to(home / ".uagent/web/devices.json")
            assert_true(client.request(asset)[0] == 404, "asset read followed a symlink")


def test_web_ask_shows_option_images_as_session_assets(root, home, *, binary):
    """An option's image is snapshotted into the session: the browser gets an
    asset it can load, never the workspace path alone."""
    workspace = root / "workspace"
    (workspace / "mockups").mkdir(parents=True)
    (workspace / "mockups" / "cards.png").write_bytes(PNG)
    questions = [
        {
            "question": "Which layout?",
            "header": "Layout",
            "options": [
                {
                    "label": "Cards",
                    "description": "a grid of cards",
                    "image": "mockups/cards.png",
                    "preview": "[#][#]",
                },
                {"label": "List", "description": "one row each"},
            ],
        }
    ]

    def responder(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "done"})
        return tool_call("ask", {"questions": questions})

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(workspace)
            client.command("submit", session, text="Pick a layout")
            value = client.until(session, lambda value: bool(value.get("pending")))
            option = value["pending"]["questions"][0]["options"][0]
            assert_true(option["preview"] == "[#][#]", option)
            asset = option["image"]["id"]
            status, body, headers = client.request(f"/api/sessions/{session['id']}/assets/{asset}")
            assert_true(status == 200 and body == PNG, (status, headers))
            client.command(
                "reply",
                session,
                interaction_id=value["pending"]["id"],
                text=json.dumps([{"choices": ["Cards"], "other": ""}]),
            )
            client.until(session, lambda value: not value.get("pending"))


def test_web_approval_interrupt_and_independent_workers(root, home, *, binary):
    first = root / "first"
    second = root / "second"
    first.mkdir()
    second.mkdir()

    continued = threading.Event()
    release = threading.Event()

    def responder(_, body):
        messages = body["messages"]
        content = json.dumps(messages)
        if "CONTINUE_AFTER_INTERRUPT" in content:
            continued.set()
            release.wait(timeout=budget(10))
            return event({"content": "continued successfully"})
        if "WEB_SECOND_WORKSPACE" in content:
            return event({"content": "other workspace works"})
        if any(message.get("role") == "tool" for message in messages):
            return event({"content": "done"})
        return tool_call("run", {"command": "sleep 30"}, call_id="long-run")

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(first)
            parallel = client.create(first)
            client.command("submit", session, text="Run the test command")
            value = client.until(session, lambda value: bool(value.get("pending")))
            assert_true(value["pending"]["approval"]["preview"], value)
            client.command("permissions", session, mode="yolo")
            assert_true(
                client.snapshot(session)["pending"]["id"] == value["pending"]["id"],
                "permission change answered pending approval",
            )
            client.command("permissions", session, mode="ask")
            client.command("submit", parallel, text="WEB_SECOND_WORKSPACE")
            client.until(
                parallel,
                lambda value: (
                    "other workspace works" in json.dumps(value)
                    and not value["metadata"]["turn_active"]
                ),
            )
            decision = value["pending"]["id"]
            client.command("reply", session, interaction_id=decision, text="y")
            wait_until(
                lambda: not client.snapshot(session)["pending"],
                "approval did not resolve",
                timeout=5,
            )
            started = time.monotonic()
            client.command("interrupt", session)
            ended = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            assert_true(
                time.monotonic() - started < budget(8), "interrupt waited for the tool timeout"
            )
            assert_true("interrupted" in json.dumps(ended), ended)
            assert_true(
                client.snapshot(parallel)["metadata"]["status"] == "idle",
                "another worker was stopped",
            )
            client.command("submit", session, text="CONTINUE_AFTER_INTERRUPT")
            try:
                assert_true(
                    continued.wait(timeout=budget(5)), "continuation never reached the model"
                )
                # Provider receipt and worker IPC are independent observations;
                # wait for the corresponding phase event to reach the master.
                active = client.until(
                    session, lambda value: value["state"].get("turn_status") == "Waiting for model"
                )
                assert_true(active["metadata"]["turn_active"], active)
                assert_true(not active["state"].get("error"), active["state"].get("error"))
                assert_true(active["state"]["turn_status"] == "Waiting for model", active["state"])
                assert_true(not active["state"].get("stop"), active["state"].get("stop"))
            finally:
                release.set()
            client.until(
                session,
                lambda value: (
                    "continued successfully" in json.dumps(value)
                    and not value["metadata"]["turn_active"]
                ),
            )
            snapshots = [path for path in session_files(home) if path.name.startswith("web-")]
            assert_true(len(snapshots) == 2, snapshots)


def test_web_side_conversation_uses_tools_and_keeps_followups(root, home, *, binary):
    started = threading.Event()
    release = threading.Event()
    offered = []

    def responder(_, body):
        messages = body["messages"]
        last = str(messages[-1].get("content", ""))
        if "Long parent task" == last:
            started.set()
            release.wait(timeout=budget(15))
            return event({"content": "PARENT-DONE"})
        offered.append({tool["function"]["name"] for tool in body.get("tools", [])})
        if "Side followup" == last:
            assert_true(any("SIDE-TOOL-DONE" in str(m.get("content")) for m in messages), messages)
            return event({"content": "SIDE-FOLLOWUP-DONE"})
        if messages[-1]["role"] == "tool":
            assert_true("side-proof" in last, last)
            return event({"content": "SIDE-TOOL-DONE"})
        return tool_call("read_path", {"path": "side-proof.txt"}, call_id="side-read")

    (root / "side-proof.txt").write_text("side-proof")
    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            parent = client.create(root)
            client.command("tools", parent, operation="set", name="run", active=False)
            client.command("submit", parent, text="Long parent task")
            assert_true(started.wait(timeout=budget(5)), "parent did not start")
            try:
                client.command("permissions", parent, mode="yolo")
                fork = client.command("side", parent, operation="create")
                # Side creation is local and finishes without a model request.
                assert_true(not fork.get("pending"), fork)
                side = {"id": fork["result"]["id"], "generation": ""}
                side = client.command("activate", side)["session"]
                client.until(side, lambda value: value["metadata"]["status"] == "idle")
                assert_true(
                    client.command("permissions", side)["result"]["effective"] == "yolo", side
                )
                client.command("submit", side, text="Read side-proof.txt")
                value = client.until(
                    side,
                    lambda value: (
                        "SIDE-TOOL-DONE" in json.dumps(value)
                        and not value["metadata"]["turn_active"]
                    ),
                )
                assert_true(
                    "read_path" in offered[0] and all("run" not in tools for tools in offered),
                    offered,
                )
                client.command("submit", side, text="Side followup")
                client.until(
                    side,
                    lambda value: (
                        "SIDE-FOLLOWUP-DONE" in json.dumps(value)
                        and not value["metadata"]["turn_active"]
                    ),
                )
                assert_true(
                    client.snapshot(parent)["metadata"]["turn_active"], "side interrupted parent"
                )
            finally:
                release.set()
            value = client.until(
                parent,
                lambda value: (
                    "PARENT-DONE" in json.dumps(value) and not value["metadata"]["turn_active"]
                ),
            )
            transcript = json.dumps(value["state"]["view"])
            assert_true(
                "SIDE-TOOL-DONE" not in transcript and "Side followup" not in transcript, transcript
            )


def test_web_side_conversation_keeps_orchestrator_tools(root, home, *, binary):
    offered = []

    def responder(_, body):
        names = {tool["function"]["name"] for tool in body.get("tools", [])}
        offered.append(names)
        if body["messages"][-1]["role"] == "tool":
            assert_true(
                not str(body["messages"][-1].get("content", "")).startswith("error:"),
                body["messages"][-1],
            )
            return event({"content": "SIDE-PANEL-DONE"})
        return tool_call("plan", {"action": "show"}, call_id="side-panel-plan")

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            panel = client.command("create", cwd=str(root), panel=True)["session"]
            panel = client.command("activate", panel)["session"]
            client.until(panel, lambda value: value["metadata"]["status"] == "idle")
            client.command("tools", panel, operation="set", name="thread", active=False)
            side_id = client.command("side", panel, operation="create")["result"]["id"]
            side = client.command("activate", {"id": side_id, "generation": ""})["session"]
            client.until(side, lambda value: value["metadata"]["status"] == "idle")
            client.command("submit", side, text="Check the panel state")
            client.until(
                side,
                lambda value: (
                    "SIDE-PANEL-DONE" in json.dumps(value) and not value["metadata"]["turn_active"]
                ),
            )
            assert_true(
                {"plan", "decide", "history", "artifact"} <= offered[0]
                and "thread" not in offered[0],
                offered,
            )
            assert_true(not {"run", "write_file", "scratch"} & offered[0], offered)


def test_web_orchestrator_can_share_an_artifact(root, home, *, binary):
    report = b"<html><body>MODERATOR-REPORT</body></html>"
    (root / "report.html").write_bytes(report)
    with Server(
        [tool_call("artifact", {"path": "report.html"}), event({"content": "REPORT-SHARED"})]
    ) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            panel = client.command("create", cwd=str(root), panel=True)["session"]
            panel = client.command("activate", panel)["session"]
            client.until(panel, lambda value: value["metadata"]["status"] == "idle")
            client.command("submit", panel, text="Share the completed report")
            value = client.until(
                panel,
                lambda value: (
                    "REPORT-SHARED" in json.dumps(value) and not value["metadata"]["turn_active"]
                ),
            )
            names = {tool["function"]["name"] for tool in provider.requests[0][1].get("tools", [])}
            assert_true("artifact" in names, names)
            assert_true(not {"write_file", "edit_file", "scratch"} & names, names)
            files = [
                part
                for block in value["state"]["view"]["blocks"]
                for part in block.get("parts") or []
                if part["kind"] == "file"
            ]
            assert_true([file["name"] for file in files] == ["report.html"], files)
            url = f"/api/sessions/{panel['id']}/assets/{files[0]['id']}?download=1"
            status, body, headers = client.request(url)
            assert_true(status == 200 and body == report, (status, body))
            assert_true('filename="report.html"' in headers.get("Content-Disposition", ""), headers)


def test_web_artifact_is_shared_sandboxed_and_downloadable(root, home, *, binary):
    (root / "report.html").write_text("<script>document.title='x'</script>REPORT")
    (root / "picture.svg").write_text(
        '<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>'
    )

    def responder(_, body):
        shared = sum(message.get("role") == "tool" for message in body["messages"])
        if shared == 2:
            return event({"content": "shared"})
        path = ["report.html", "picture.svg"][shared]
        return tool_call("artifact", {"path": path}, call_id=f"share-{shared}")

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            client.command("permissions", session, mode="yolo")
            client.command("submit", session, text="Share the report")
            value = client.until(
                session,
                lambda value: (
                    "shared" in json.dumps(value) and not value["metadata"]["turn_active"]
                ),
            )
            files = [
                part
                for block in value["state"]["view"]["blocks"]
                for part in block.get("parts") or []
                if part["kind"] == "file"
            ]
            assert_true([file["name"] for file in files] == ["report.html", "picture.svg"], files)
            # An SVG is an image, but its script must never run on this origin.
            status, _, headers = client.request(
                f"/api/sessions/{session['id']}/assets/{files[1]['id']}"
            )
            policy = headers.get("Content-Security-Policy", "")
            assert_true(status == 200 and "sandbox" in policy, headers)
            assert_true("allow-scripts" not in policy, headers)
            url = f"/api/sessions/{session['id']}/assets/{files[0]['id']}"
            status, body, headers = client.request(url)
            assert_true(status == 200 and b"REPORT" in body, (status, body))
            # Opens inline, but scripts run in an opaque origin.
            assert_true(headers.get("Content-Type", "").startswith("text/html"), headers)
            assert_true("sandbox" in headers.get("Content-Security-Policy", ""), headers)
            assert_true("allow-same-origin" not in headers["Content-Security-Policy"], headers)
            assert_true(headers.get("X-Content-Type-Options") == "nosniff", headers)
            status, _, headers = client.request(url + "?download=1")
            assert_true(
                status == 200
                and 'filename="report.html"' in headers.get("Content-Disposition", "")
                and "filename*=UTF-8''report.html" in headers["Content-Disposition"],
                headers,
            )


def test_web_rows_show_output_while_the_model_reads_hints(root, home, *, binary):
    """A result's hints reach the model and never the rows a person reads."""
    (root / "notes.txt").write_text("first\nsecond\n", encoding="utf-8")
    told = []

    def responder(_, body):
        if not any(message.get("role") == "tool" for message in body["messages"]):
            return tool_calls(
                [
                    ("run-1", "run", {"command": "printf 'visible-out\\n'; exit 3"}),
                    ("read-1", "read_path", {"path": "notes.txt"}),
                ]
            )
        told.extend(tool_results(body["messages"]))
        return event({"content": "hints-done"})

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            client.command("permissions", session, mode="yolo")
            client.command("submit", session, text="Run and read")
            value = client.until(
                session,
                lambda value: (
                    "hints-done" in json.dumps(value) and not value["metadata"]["turn_active"]
                ),
            )
    # The model reads each output, then its hint.
    assert_true(len(told) == 2, told)
    assert_true(told[0].startswith("error: visible-out\n") and "[exit code 3]" in told[0], told)
    assert_true(told[1].startswith("first\nsecond\n") and "lines 1-2 of 2]" in told[1], told)
    ran, read = [block for block in value["state"]["view"]["blocks"] if block.get("kind") == "call"]
    # A person reads the output and the facts, worded once by the host.
    assert_true(ran["body"]["text"] == "error: visible-out\n", ran)
    assert_true(ran["state"] == "failed" and ran["result"]["exit"] == {"code": 3}, ran)
    assert_true(ran["result"]["word"].startswith("exit 3 · "), ran)
    assert_true(ran["body"]["kind"] == "tail" and ran["headline"]["verb"] == "Ran", ran)
    assert_true(read["body"]["text"] == "first\nsecond\n", read)
    assert_true(read["state"] == "ok" and read["result"]["word"].startswith("2 lines"), read)
    assert_true("[" not in ran["body"]["text"] + read["body"]["text"], (ran, read))
    assert_true("exchange_path" not in ran and "status" not in ran, ran)


def test_web_long_diff_is_stored_whole_and_served_by_detail(root, home, *, binary):
    lines = [f"line {index:04d} of a long file" for index in range(2000)]
    content = "\n".join([*lines, "LAST_DIFF_LINE", ""])
    # The turn waits after the write, so the diff is read while it runs and
    # again once it is saved.
    release = threading.Event()

    def responder(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            release.wait(budget(10))
            return event({"content": "written"})
        return tool_call("write_file", {"path": "long.txt", "content": content}, call_id="long")

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            client.command("permissions", session, mode="yolo")
            client.command("submit", session, text="Write the long file")

            def diff_row(value):
                return next(
                    (
                        block
                        for block in value["state"]["view"]["blocks"]
                        if block.get("body", {}).get("kind") == "diff"
                    ),
                    None,
                )

            def whole(row):
                diff = ""
                offset = 0
                while True:
                    status, page, _ = client.json(
                        f"/api/sessions/{session['id']}?detail={row['id']}&offset={offset}"
                    )
                    assert_true(status == 200, page)
                    diff += page["text"]
                    if not page["more"]:
                        return diff
                    offset = page["next"]

            try:
                value = client.until(
                    session,
                    lambda value: diff_row(value) is not None and value["metadata"]["turn_active"],
                )
                row = diff_row(value)
                # Its path stays with the host, which serves the whole diff
                # while the turn runs.
                assert_true("change_path" not in row, row)
                live = whole(row)
            finally:
                release.set()
            value = client.until(
                session,
                lambda value: (
                    "written" in json.dumps(value) and not value["metadata"]["turn_active"]
                ),
            )
            # It is kept with the session's edits, so it lasts as long as the
            # conversation and leaves with it.
            stored = list((home / ".uagent" / "history").rglob("*.json.edits/diff-*"))
            assert_true(len(stored) == 1, stored)
            # The row carries the diff's opening, within a fact and a frame.
            # The counts are facts; the diff holds only its lines.
            row = diff_row(value)
            change = row["body"]["text"]
            assert_true(row["body"]["kind"] == "diff" and row["body"]["truncated"], row)
            assert_true(change.startswith("+line 0000 of a long file\n"), row)
            assert_true(row["result"]["added"] == 2001 and row["result"]["removed"] == 0, row)
            assert_true(len(change) <= 16 * 1024 and "…" not in change, row)
            # The same whole diff, live and saved.
            diff = whole(row)
            assert_true(live == diff, (len(live), len(diff)))
            assert_true(diff.startswith(change), diff[:200])
            assert_true(diff.endswith("+line 1999 of a long file\n+LAST_DIFF_LINE\n"), diff[-200:])
            assert_true(diff.count("\n") == 2001, diff.count("\n"))
