import base64
import json
import os
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path

from integration_support import (
    Server,
    assert_true,
    budget,
    event,
    save_settings,
    saved_settings,
)
from web_support import WebClient, web_host

from integration_web._common import PNG


def test_web_restart_durable_images_and_read_only_catalogue(root, home, *, binary):
    project = root / "durable-images"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "Durable image answer"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            saved = client.create(project)
            status, asset, _ = client.json(
                f"/api/sessions/{saved['id']}/attachments",
                raw=PNG,
                headers={"Content-Type": "image/png"},
            )
            assert_true(status == 200, asset)
            client.command("submit", saved, text="Keep this original", attachment_ids=[asset["id"]])
            client.until(
                saved,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and "Durable image answer" in json.dumps(value)
                ),
            )
            draft = client.create(project, activate=False)
            cookie = client.cookie
        with web_host(binary, root, home, provider.url) as (client, _, _, _):
            client.cookie = cookie
            listing = client.json("/api/sessions")[1]
            assert_true(
                {saved["id"], draft["id"]} <= {item["id"] for item in listing["sessions"]}, listing
            )
            assert_true(
                all(not item["generation"] for item in listing["sessions"]),
                "opening history created a worker",
            )
            value = client.snapshot(saved)
            assert_true(asset["id"] in json.dumps(value), value)
            assert_true(
                client.request(f"/api/sessions/{saved['id']}/assets/{asset['id']}")[1] == PNG,
                "restart changed original bytes",
            )
            private = client.json(f"/api/sessions/{saved['id']}?detail=m-1")[1]
            assert_true(private["text"] == "", "internal message exposed")
            active = client.command("activate", saved)["session"]
            client.until(active, lambda value: value["metadata"]["status"] == "idle")
            client.command(
                "submit", active, text="Inspect this original again", attachment_ids=[asset["id"]]
            )
            client.until(active, lambda value: value["metadata"]["status"] == "idle")
            assert_true(len(provider.requests) == 2, provider.requests)
            image_parts = [
                part
                for message in provider.requests[-1][1]["messages"]
                if isinstance(message.get("content"), list)
                for part in message["content"]
                if part.get("type") == "image_url"
            ]
            assert_true(len(image_parts) == 1, provider.requests[-1])
            assert_true(
                base64.b64decode(image_parts[0]["image_url"]["url"].split(",", 1)[1]) == PNG,
                "resume did not preserve the original image",
            )


def test_web_concurrent_retry_and_resync(root, home, *, binary):
    project = root / "retry"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "One accepted submission"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            payload = {
                "v": 2,
                "kind": "submit",
                "request_id": "a" * 32,
                "session_id": session["id"],
                "generation": session["generation"],
                "text": "Once only",
            }
            results = []
            gate = threading.Barrier(3)

            def send():
                peer = WebClient(client.port)
                peer.cookie = client.cookie
                gate.wait()
                results.append(peer.json("/api/command", payload)[0])

            threads = [threading.Thread(target=send) for _ in range(2)]
            for thread in threads:
                thread.start()
            gate.wait()
            for thread in threads:
                thread.join(timeout=budget(10))
            assert_true(results == [200, 200], results)
            client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and "One accepted submission" in json.dumps(value)
                ),
            )
            assert_true(len(provider.requests) == 1, provider.requests)
            status, body, _ = client.request("/api/events?cursor=obsolete:999")
            assert_true(status == 200 and b"event: resync" in body, body)


def test_web_project_trust_and_config_precedence(root, home, *, binary):
    """What is saved for a project applies to its conversations, over what is
    saved for all; nothing is asked and nothing is read from the project."""
    first, second = root / "overriding", root / "plain"
    for project in (first, second):
        project.mkdir()
    save_settings(home, binary=binary, UAGENT_MODEL="global/model")
    save_settings(home, folder=first, binary=binary, UAGENT_MODEL="project/model")
    with Server([lambda _, _body: event({"content": "Configured"})]) as provider:
        with web_host(binary, root, home, provider.url, extra_env={"UAGENT_MODEL": None}) as (
            client,
            code,
            _,
            _,
        ):
            client.pair(code)
            # The settings screen saves for a project by naming its folder.
            client.command(
                "config",
                operation="apply",
                scope="project",
                cwd=str(second),
                changes=[{"key": "UAGENT_MAX_STEPS", "value": "7"}],
            )
            assert_true(saved_settings(home, second) == {"limits.maxSteps": 7}, "not saved")
            assert_true(saved_settings(home) == {"model": "global/model"}, "leaked")
            for project, expected in ((first, "project/model"), (second, "global/model")):
                session = client.create(project)
                value = client.until(session, lambda value: value["metadata"]["status"] == "idle")
                assert_true(not value.get("pending"), value)
                assert_true(value["state"]["route"] == expected, value)


def test_session_runtime_crash_isolation(root, home, *, binary):
    with Server([lambda _, _body: event({"content": "Unaffected worker"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, process, _):
            client.pair(code)
            sessions = []
            for index in range(5):
                project = root / f"workspace-{index}"
                project.mkdir()
                sessions.append(client.create(project))
            rows = subprocess.check_output(["ps", "-ax", "-o", "pid=,ppid=,args="], text=True)
            victim = next(
                int(row.split()[0])
                for row in rows.splitlines()
                if row.split()[1] == str(process.pid) and str(root / "workspace-0") in row
            )
            os.kill(victim, signal.SIGKILL)
            # It had nothing under way, so nothing was interrupted: it is saved.
            client.until(sessions[0], lambda value: value["metadata"]["status"] == "saved")
            client.command("submit", sessions[1], text="Continue independently")
            client.until(sessions[1], lambda value: "Unaffected worker" in json.dumps(value))
            resumed = client.command("activate", sessions[0])["session"]
            assert_true(resumed["generation"] != sessions[0]["generation"], resumed)
            client.until(resumed, lambda value: value["metadata"]["status"] == "idle")


def test_web_slow_upload_does_not_block_controls(root, home, *, binary):
    project = root / "slow-upload"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "Image accepted"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            endpoint = f"/api/sessions/{session['id']}/attachments"
            status, image, _ = client.json(endpoint, raw=PNG, headers={"Content-Type": "image/png"})
            assert_true(status == 200, image)
            with socket.create_connection(("127.0.0.1", client.port)) as slow:
                headers = (
                    f"POST {endpoint} HTTP/1.1\r\nHost: 127.0.0.1:{client.port}\r\n"
                    f"Origin: {client.origin}\r\nCookie: {client.cookie}\r\n"
                    "Content-Type: image/png\r\nContent-Length: 1000\r\n\r\n"
                )
                slow.sendall(headers.encode() + PNG[:16])
                time.sleep(0.1)
                result = []
                thread = threading.Thread(
                    target=lambda: result.append(
                        client.json(
                            "/api/command",
                            {
                                "v": 2,
                                "request_id": "c" * 32,
                                "kind": "submit",
                                "session_id": session["id"],
                                "generation": session["generation"],
                                "text": "Image after upload",
                                "attachment_ids": [image["id"]],
                            },
                        )
                    )
                )
                thread.start()
                try:
                    time.sleep(0.1)
                    started = time.monotonic()
                    client.command("interrupt", session)
                    assert_true(time.monotonic() - started < budget(1.5), "upload blocked Stop")
                finally:
                    slow.shutdown(socket.SHUT_RDWR)
                    thread.join(timeout=budget(10))
                assert_true(result and result[0][0] == 200, result)


def test_web_save_failure_remains_visible(root, home, *, binary):
    project = root / "save-failure"
    project.mkdir()
    with Server([lambda _, _body: event({"content": "Still in memory"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            record = json.loads((home / f".uagent/web/drafts/{session['id']}.json").read_text())
            Path(record["path"]).unlink()
            Path(record["path"]).mkdir()
            client.command("submit", session, text="Exercise a failed checkpoint")
            value = client.until(
                session, lambda value: "cannot save session" in value["state"].get("error", "")
            )
            assert_true("Still in memory" in json.dumps(value), value)
            assert_true(len(provider.requests) == 1, provider.requests)
