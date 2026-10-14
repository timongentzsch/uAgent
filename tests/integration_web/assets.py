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
                    session, lambda value: value["state"].get("activity") == "Waiting for model"
                )
                assert_true(active["metadata"]["turn_active"], active)
                assert_true(not active["state"].get("error"), active["state"].get("error"))
                assert_true(active["state"]["activity"] == "Waiting for model", active["state"])
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


def test_web_side_question_answers_beside_a_running_turn(root, home, *, binary):
    started = threading.Event()
    release = threading.Event()

    def responder(_, body):
        last = str(body["messages"][-1].get("content", ""))
        if "Side question" in last:
            # Same conversation prefix, no new tools offered beyond the turn's.
            return event({"content": "SIDE-ANSWER"})
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "MAIN-DONE"})
        started.set()
        release.wait(timeout=budget(10))
        return tool_call("run", {"command": "true"}, call_id="main-run")

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            client.command("permissions", session, mode="yolo")
            client.command("submit", session, text="Long main task")
            assert_true(started.wait(timeout=budget(5)), "main turn never started")
            try:
                side = client.command("side", session, text="what are we doing?")
                answer = side.get("result", {}).get("answer")
                if side.get("pending"):
                    status, receipt, _ = client.json("/api/receipts/" + side["request_id"])
                    answer = receipt.get("result", {}).get("answer")
                assert_true(answer == "SIDE-ANSWER", side)
            finally:
                release.set()
            value = client.until(
                session,
                lambda value: (
                    "MAIN-DONE" in json.dumps(value) and not value["metadata"]["turn_active"]
                ),
            )
            transcript = json.dumps(value["state"]["view"])
            assert_true("SIDE-ANSWER" not in transcript, transcript)
            assert_true("what are we doing" not in transcript, transcript)


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


def test_web_long_diff_is_stored_whole_and_served_by_detail(root, home, *, binary):
    lines = [f"line {index:04d} of a long file" for index in range(2000)]
    content = "\n".join([*lines, "LAST_DIFF_LINE", ""])

    def responder(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "written"})
        return tool_call("write_file", {"path": "long.txt", "content": content}, call_id="long")

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            client.command("permissions", session, mode="yolo")
            client.command("submit", session, text="Write the long file")
            value = client.until(
                session,
                lambda value: (
                    "written" in json.dumps(value) and not value["metadata"]["turn_active"]
                ),
            )
            row = next(
                block for block in value["state"]["view"]["blocks"] if block.get("change_path")
            )
            # It is kept with the session's edits, so it lasts as long as the
            # conversation and leaves with it.
            assert_true(".json.edits/diff-" in row["change_path"], row["change_path"])
            # The row carries the diff's opening, within a fact and a frame.
            assert_true(row["change"].startswith("Created long.txt (+2001 -0)\n"), row)
            assert_true(len(row["change"]) <= 16 * 1024 and "…" not in row["change"], row)
            diff = ""
            offset = 0
            while True:
                status, page, _ = client.json(
                    f"/api/sessions/{session['id']}?detail={row['detail_id']}&offset={offset}"
                )
                assert_true(status == 200, page)
                diff += page["text"]
                if not page["more"]:
                    break
                offset = page["next"]
            assert_true(diff.startswith(row["change"]), diff[:200])
            assert_true(diff.endswith("+line 1999 of a long file\n+LAST_DIFF_LINE\n"), diff[-200:])
            assert_true(diff.count("\n") == 2002, diff.count("\n"))
