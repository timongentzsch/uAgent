import gzip
import json
import threading
import time
from pathlib import Path

from integration_support import (
    Server,
    assert_true,
    budget,
    event,
    wait_until,
    write_json_response,
)
from web_support import web_host

from integration_web._common import ASSET_NAME_CHARS


def test_web_http_context_configuration_permissions_and_fork(root, home, *, binary):
    project = root / "context-fork"
    project.mkdir()
    answer = "Exact raw answer α🙂"
    with Server([lambda _, _body: event({"content": answer})]) as provider:
        with web_host(
            binary, root, home, provider.url, extra_env={"UAGENT_API_KEY": "private-http-test-key"}
        ) as (client, code, _, _):
            client.pair(code)
            config = client.command("config")["result"]
            settings = {item["name"]: item for item in config["settings"]}
            assert_true(
                len(settings) > 60 and "effective" not in settings["UAGENT_API_KEY"], config
            )
            changed = client.command(
                "config",
                operation="apply",
                scope="user",
                changes=[{"key": "UAGENT_APPROVAL", "value": "yolo"}],
            )["result"]
            assert_true(
                changed["effects"][0]["effect"] == "next_turn",
                changed["effects"],
            )
            session = client.create(project)
            initial = client.snapshot(session)
            assert_true(
                initial["state"]["permissions"]
                == {"mode": "default", "effective": "yolo", "default": "yolo"},
                initial,
            )
            client.command("permissions", session, mode="ask")
            # The conversation's mode is the approval setting, chosen at its
            # scope over the one saved for all conversations.
            approval = next(
                item
                for item in client.command("config", session)["result"]["settings"]
                if item["name"] == "UAGENT_APPROVAL"
            )
            assert_true(
                approval["source"] == "conversation"
                and approval["set"] == {"user": "yolo", "conversation": "ask"}
                and approval["scopes"] == ["user", "project", "conversation"]
                and approval["effective"] == "ask",
                approval,
            )
            assert_true(
                client.snapshot(session)["state"]["permissions"]
                == {"mode": "ask", "effective": "ask", "default": "yolo"},
                "conversation mode",
            )

            # One path for every scope: the same command chooses for the
            # conversation, and unsetting takes the choice back.
            def choose(**change):
                return client.command(
                    "config",
                    session,
                    operation="apply",
                    scope="conversation",
                    changes=[{"key": "UAGENT_APPROVAL", **change}],
                )["result"]

            choose(value="auto")
            assert_true(
                client.snapshot(session)["state"]["permissions"]
                == {"mode": "auto", "effective": "auto", "default": "yolo"},
                "chosen through config",
            )
            choose(unset=True)
            assert_true(
                client.snapshot(session)["state"]["permissions"]
                == {"mode": "default", "effective": "yolo", "default": "yolo"},
                "choice taken back",
            )
            status, refused, _ = client.json(
                "/api/command",
                {
                    "v": 2,
                    "kind": "config",
                    "request_id": "e" * 32,
                    "session_id": session["id"],
                    "generation": "",
                    "operation": "apply",
                    "scope": "conversation",
                    "changes": [{"key": "UAGENT_MAX_STEPS", "value": "9"}],
                },
            )
            assert_true("cannot be set for one conversation" in json.dumps(refused), refused)
            client.command("permissions", session, mode="ask")
            preview = client.command("context", session)["result"]["exchanges"][0]
            assert_true(preview["preview"] and not provider.requests, preview)

            def body_for(target, exchange, part):
                text, offset, metadata = "", 0, None
                while True:
                    status, page, _ = client.json(
                        f"/api/sessions/{target['id']}?http={exchange['id']}&part={part}&offset={offset}"
                    )
                    assert_true(status == 200, page)
                    text += page["text"]
                    metadata = page["exchange"]
                    if not page["more"]:
                        return text, metadata
                    assert_true(page["next"] > offset, page)
                    offset = page["next"]

            prepared, _ = body_for(session, preview, "request")
            assert_true("tools" in json.loads(prepared), prepared[:100])
            data = b"\x00\xffarbitrary binary\x00"
            uploaded_name = "x" * 200 + ".unknown"
            status, asset, _ = client.json(
                f"/api/sessions/{session['id']}/attachments?name={uploaded_name}",
                raw=data,
                headers={"Content-Type": "application/octet-stream"},
            )
            assert_true(
                status == 200
                and asset["name"] == uploaded_name[:ASSET_NAME_CHARS]
                and not asset["image"],
                asset,
            )
            client.command(
                "submit",
                session,
                text="Read attached bytes " + "α🙂" * 3500,
                attachment_ids=[asset["id"]],
            )
            snapshot = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            rows = snapshot["state"]["view"]["blocks"]
            request = rows[0]
            # Clients get the asset id, never the private upload path.
            assert_true(request.get("files"), request)
            assert_true(all("path" not in item for item in request["files"]), request)
            assert_true("Attached:" not in request["text"], request["text"])
            reply = next(row for row in reversed(rows) if row["kind"] == "assistant")
            assert_true(reply["reply_to"] == request["id"] == reply["turn_root"], rows)
            exchange = reply["http"][-1]
            captured, metadata = body_for(session, exchange, "request")
            assert_true(
                json.loads(captured) == provider.requests[-1][1],
                "HTTP request differs from provider payload",
            )
            assert_true(
                "private-http-test-key" not in json.dumps(metadata)
                and "[redacted]" in metadata["request_headers"],
                metadata,
            )
            response, _ = body_for(session, exchange, "response")
            assert_true(
                response.startswith("data: ") and response.endswith("data: [DONE]\n\n"), response
            )
            assert_true("tools" in json.loads(captured), "tools absent from HTTP body")
            fork = client.command("fork", session, title="Independent fork")["result"]
            assert_true(fork["id"] != session["id"], fork)
            # The browser forwards the typed /fork argument; @N is a turn.
            branch = client.command("fork", session, argument="Branch @1")["result"]
            assert_true(branch["title"] == "Branch", branch)
            fork_saved = client.snapshot(fork)
            assert_true(fork_saved["metadata"]["status"] == "saved", fork_saved)
            assert_true(
                body_for(fork, exchange, "response")[0] == response, "fork lost HTTP capture"
            )
            client.command("close", session)
            session = client.snapshot(session)["metadata"]
            client.command("delete", session)
            assert_true(
                client.request(f"/api/sessions/{fork['id']}/assets/{asset['id']}")[1] == data,
                "deleting source removed fork attachment",
            )
            assert_true(
                body_for(fork, exchange, "response")[0] == response,
                "deleting source removed fork HTTP capture",
            )
            active = client.command("activate", fork)["session"]
            current = client.until(active, lambda value: value["metadata"]["status"] == "idle")
            assert_true(current["state"]["permissions"]["mode"] == "ask", current)
            client.command("submit", active, text="Continue the fork")
            client.until(active, lambda value: value["metadata"]["status"] == "idle")
            assert_true(
                answer in json.dumps(provider.requests[-1][1], ensure_ascii=False),
                "fork lost prior conversation",
            )
            assert_true(
                fork["path"] + ".assets/" in json.dumps(provider.requests[-1][1]),
                "fork references deleted source attachment",
            )
            assert_true(
                len(provider.requests) == 2, "inspection or configuration invoked the model"
            )


def test_web_http_retry_and_failed_request_persistence(root, home, *, binary):
    import http.client

    def unavailable(handler, _body):
        write_json_response(handler, {"error": {"message": "temporary unavailable"}}, status=503)

    def rejected(handler, _body):
        write_json_response(handler, {"error": {"message": "terminal rejection"}}, status=400)

    with Server([unavailable, event({"content": "Recovered after retry"}), rejected]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            client.command("submit", session, text="Retry this request")
            waiting = client.until(session, lambda value: value["state"].get("phase") == "retrying")
            caption = waiting["state"]["activity"]
            assert_true(caption == "Retrying model · attempt 2/3", caption)
            snapshot = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            reply = next(
                row
                for row in reversed(snapshot["state"]["view"]["blocks"])
                if row["kind"] == "assistant"
            )
            exchanges = reply["http"]
            assert_true([item["status"] for item in exchanges] == [503, 200], exchanges)
            first = client.json(
                f"/api/sessions/{session['id']}?http={exchanges[0]['id']}&part=response"
            )[1]
            assert_true(
                json.loads(first["text"])["error"]["message"] == "temporary unavailable", first
            )
            listing = client.json("/api/sessions")[1]
            client.command("submit", session, text="Fail this request")
            stream = http.client.HTTPConnection("127.0.0.1", client.port, timeout=15)
            try:
                stream.request(
                    "GET",
                    f"/api/events?cursor={listing['epoch']}:{listing['cursor']}",
                    headers={"Cookie": client.cookie},
                )
                response = stream.getresponse()
                assert_true(response.status == 200, response.status)
                # Replay every transition: polling can miss a premature idle state.
                while line := response.readline():
                    if not line.startswith(b"data: "):
                        continue
                    frame = json.loads(line[6:])
                    if frame.get("session_id") != session["id"]:
                        continue
                    if frame.get("kind") == "state" and not frame["busy"]:
                        assert_true(frame["checkpoint"], "idle preceded the final checkpoint")
                        break
                else:
                    raise AssertionError("worker stream ended before the final checkpoint")
            finally:
                stream.close()
            snapshot = client.until(session, lambda value: value["metadata"]["status"] == "idle")
            exchange = snapshot["state"]["http"][-1]
            failed_user = next(
                block
                for block in snapshot["state"]["view"]["blocks"]
                if block["text"] == "Fail this request"
            )
            assert_true(
                failed_user["http"][-1]["id"] == exchange["id"],
                "failed HTTP call has no durable request link",
            )
            assert_true(exchange["status"] == 400 and exchange["state"] == "failed", exchange)
            client.command("close", session)
            saved = client.json(
                f"/api/sessions/{session['id']}?http={exchange['id']}&part=response"
            )[1]
            assert_true(
                json.loads(saved["text"])["error"]["message"] == "terminal rejection", saved
            )
            assert_true(
                Path(saved["exchange"]["response_path"]).stat().st_mode & 0o777 == 0o600,
                "HTTP body is not private",
            )
            assert_true(len(provider.requests) == 3, provider.requests)


def test_web_slash_registry_and_attachment_retention(root, home, *, binary):
    attached = root / "slash-note.txt"
    attached.write_text("RETAINED_SLASH_ATTACHMENT")
    with Server([event({"content": "Attachment received"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            catalog = client.json("/api/sessions")[1]
            help_command = next(row for row in catalog["commands"] if row["command"] == "/help")
            assert_true("/commands" in help_command["aliases"], help_command)
            session = client.create(root)
            client.command("submit", session, text=f"/attach {attached}")
            client.until(
                session,
                lambda value: (
                    value["state"].get("attachments") == 1 and value["metadata"]["status"] == "idle"
                ),
            )
            client.command("submit", session, text="/status")
            client.until(
                session,
                lambda value: (
                    value["state"].get("attachments") == 1 and value["metadata"]["status"] == "idle"
                ),
            )
            client.command("submit", session, text="Read the attachment")
            client.until(
                session,
                lambda value: (
                    value["state"].get("attachments") == 0 and value["metadata"]["status"] == "idle"
                ),
            )
            assert_true(len(provider.requests) == 1, provider.requests)
            content = provider.requests[0][1]["messages"][-1]["content"]
            text = "\n".join(block.get("text", "") for block in content)
            assert_true(attached.name in text and attached.read_text() in text, content)
            assert_true(not any(block["type"] == "file" for block in content), content)


def test_web_session_title_generation_respects_rename(root, home, *, binary):
    release = threading.Event()
    served = []

    def route(_, body):
        if body.get("model") != "titler":
            return event({"content": "Recorded answer"})
        # A title for the second session waits until the user has renamed it.
        if "rename me" in json.dumps(body["messages"]):
            release.wait(budget(10))
        served.append(body)
        return event({"content": '"Investigate browser efficiency."'})

    with Server([route]) as provider:
        with web_host(
            binary, root, home, provider.url, extra_env={"UAGENT_TITLE_MODEL": "titler"}
        ) as (client, code, _, _):
            client.pair(code)
            named = client.create(root)
            client.command("submit", named, text="why is the browser slow on this page")
            # Quotes and the trailing period are stripped from the model's answer.
            client.until(
                named,
                lambda value: value["metadata"]["title"] == "Investigate browser efficiency",
            )
            kept = client.create(root)
            client.command("submit", kept, text="please rename me afterwards")
            client.until(kept, lambda value: value["metadata"]["status"] == "idle")
            client.command("rename", kept, title="Mine")
            release.set()
            wait_until(lambda: len(served) == 2, lambda: served)
            # The late title reaches the runtime and must not replace the rename.
            time.sleep(0.5)
            title = client.snapshot(kept)["metadata"]["title"]
            assert_true(title == "Mine", title)


def test_the_title_is_written_by_the_conversations_model(root, home, *, binary):
    """With no title model set, the session's own model names it."""
    models = []

    def route(_, body):
        if "Title this coding session" not in json.dumps(body["messages"]):
            return event({"content": "Recorded answer"})
        models.append(body.get("model"))
        return event({"content": "Named by the session model"})

    with Server([route]) as provider:
        env = {"UAGENT_TITLE_MODEL": "", "UAGENT_MODEL": "session-model"}
        with web_host(binary, root, home, provider.url, extra_env=env) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            client.command("submit", session, text="why is the browser slow on this page")
            client.until(
                session, lambda value: value["metadata"]["title"] == "Named by the session model"
            )
            assert_true(models == ["session-model"], models)


def test_web_bundle_is_served_gzipped_and_plain_to_a_client_that_cannot_take_it(
    root, home, *, binary
):
    dist = Path(__file__).resolve().parents[2] / "web/dist"
    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, _, _, _):
            for path, mime in (
                ("/", "text/html"),
                ("/sw.js", "text/javascript"),
                ("/manifest.webmanifest", "application/manifest+json"),
            ):
                expected = (dist / (path.lstrip("/") or "index.html")).read_bytes()
                status, body, headers = client.request(
                    path, headers={"Accept-Encoding": "gzip, deflate"}
                )
                assert_true(status == 200 and headers["Content-Encoding"] == "gzip", headers)
                assert_true(headers["Vary"] == "Accept-Encoding", headers)
                assert_true(headers["Content-Type"].startswith(mime), headers)
                assert_true(gzip.decompress(body) == expected, f"{path} differs from web/dist")
                status, body, headers = client.request(path)
                assert_true(status == 200 and "Content-Encoding" not in headers, headers)
                assert_true(headers["Vary"] == "Accept-Encoding", headers)
                assert_true(body == expected, f"{path} plain body differs from web/dist")
            expected = (dist / "sw.js").read_bytes()
            # Only an acceptable gzip entry gets gzip: a refusal, a longer
            # token or a wildcard that excludes it does not.
            for accept, gzipped in (
                ("GZIP", True),
                ("deflate, Gzip;q=0.5", True),
                ("*;q=1, identity;q=0", True),
                ("*", True),
                ("gzip;q=0", False),
                ("gzip;q=0, *", False),
                ("xgzip", False),
                ("*;q=0", False),
                ("identity", False),
            ):
                status, body, headers = client.request(
                    "/sw.js", headers={"Accept-Encoding": accept}
                )
                assert_true(status == 200, accept)
                assert_true(("Content-Encoding" in headers) == gzipped, (accept, headers))
                assert_true((gzip.decompress(body) if gzipped else body) == expected, accept)
            # A range selects bytes of the plain asset, never of a gzip stream.
            status, body, headers = client.request(
                "/sw.js", headers={"Accept-Encoding": "gzip", "Range": "bytes=0-9"}
            )
            assert_true(status == 206 and "Content-Encoding" not in headers, headers)
            assert_true(body == expected[:10], body)
            status, body, headers = client.request(
                "/sw.js", headers={"Accept-Encoding": "gzip", "Range": "bytes=0-9,20-29"}
            )
            assert_true(status == 206 and "Content-Encoding" not in headers, headers)
            assert_true(
                headers["Content-Type"].startswith("multipart/byteranges")
                and expected[:10] in body
                and expected[20:30] in body,
                headers,
            )
            # Already-compressed types are embedded raw and never relabelled.
            status, body, headers = client.request(
                "/icon-192.png", headers={"Accept-Encoding": "gzip"}
            )
            assert_true(
                status == 200
                and "Content-Encoding" not in headers
                and body == (dist / "icon-192.png").read_bytes(),
                headers,
            )


def test_a_conversation_without_a_model_takes_the_one_chosen_from_the_catalogue(
    root, home, *, binary
):
    """On an endpoint named by nothing but its address, the catalogue's own
    model ids are what the picker offers and what it takes."""
    served = []

    def route(_, body):
        served.append(body.get("model"))
        return event({"content": "answered"})

    models = {"data": [{"id": "alpha"}, {"id": "beta"}]}
    with Server([route], get_response=models) as provider:
        env = {"UAGENT_MODEL": None, "UAGENT_TITLE_MODEL": "off"}
        with web_host(binary, root, home, provider.url, extra_env=env) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            assert_true(client.snapshot(session)["state"]["route"] == "", "model chosen for it")
            client.command("submit", session, text="before choosing")
            client.until(session, lambda value: "no model set" in value["state"]["error"])
            assert_true(not served, served)
            catalog = client.command("model", session, operation="catalog")["result"]
            assert_true([item["value"] for item in catalog["models"]] == ["alpha", "beta"], catalog)
            client.command("model", session, operation="select", model="beta")
            client.command("submit", session, text="after choosing")
            client.until(
                session,
                lambda value: any(
                    row["kind"] == "assistant" for row in value["state"]["view"]["blocks"]
                ),
            )
            assert_true(served == ["beta"], served)
