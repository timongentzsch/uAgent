import json

from integration_support import (
    Server,
    assert_true,
    event,
    run,
    session_files,
)
from web_support import web_host


def test_web_external_origin_pairing(root, home, *, binary):
    with Server([event({"content": "unused"})]) as provider:
        for origin in ("https://browser.example", "http://100.64.0.9:18080"):
            with web_host(
                binary, root, home, provider.url, extra_env={"UAGENT_WEB_ORIGIN": origin}
            ) as (client, code, _, env):
                client.origin = origin
                host = {"Host": origin.split("://", 1)[1]}
                status, value, headers = client.json("/api/auth", {"code": code}, headers=host)
                assert_true(status == 200, value)
                cookie = headers["Set-Cookie"]
                assert_true(("; Secure" in cookie) == origin.startswith("https://"), cookie)
                client.cookie = cookie.split(";", 1)[0]
                assert_true(client.json("/api/sessions", headers=host)[0] == 200, "pairing failed")
                assert_true(
                    client.json("/api/sessions", headers={**host, "Origin": "http://evil.example"})[
                        0
                    ]
                    == 403,
                    "foreign origin accepted",
                )
                assert_true(client.json("/api/sessions")[0] == 403, "foreign host accepted")
        for origin in (
            "http://example.com",
            "http://127.0.0.1",
            "http://100.63.255.255",
            "http://100.128.0.0",
            "http://100.64.0.9@evil.example",
            "http://100.64.0.9/path",
            "http://100.64.0.9:99999",
        ):
            rejected = run(root, env, "--web", "--web-origin", origin, binary=binary)
            assert_true(rejected.returncode != 0 and "origin" in rejected.stderr, rejected.stderr)


def test_web_external_origin_allows_authenticated_loopback(root, home, *, binary):
    with Server([event({"content": "unused"})]) as provider:
        with web_host(
            binary,
            root,
            home,
            provider.url,
            extra_env={"UAGENT_WEB_ORIGIN": "https://browser.example"},
        ) as (client, code, _, _):
            assert_true(client.request("/")[0] == 200, "loopback shell rejected")
            assert_true(client.json("/api/sessions")[0] == 401, "loopback bypassed pairing")
            status, value, headers = client.json(
                "/api/auth", {"code": code, "name": "Loopback device"}
            )
            assert_true(status == 200, value)
            cookie = headers["Set-Cookie"]
            assert_true("; Secure" not in cookie, cookie)
            client.cookie = cookie.split(";", 1)[0]
            assert_true(client.json("/api/sessions")[0] == 200, "loopback pairing failed")
            assert_true(
                client.json("/api/sessions", headers={"Origin": "http://evil.example"})[0] == 403,
                "foreign origin accepted through loopback",
            )


def test_web_singleton_auth_persistence(root, home, *, binary):
    project = root / "outside-launch"
    project.mkdir()
    (root / ".mcp.json").write_text('{"mcpServers":{"untrusted":{"command":"false"}}}')
    with Server(
        [
            lambda _, _body: event(
                {"content": "A real saved answer", "reasoning_content": "Supplied thinking"}
            )
        ]
    ) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, process, env):
            status, _, _ = client.json("/api/sessions")
            assert_true(status == 401, status)
            status, _, _ = client.json(
                "/api/auth", {"code": code}, headers={"Origin": "https://evil.example"}
            )
            assert_true(status == 403, status)
            client.pair(code)
            assert_true(client.json("/api/auth", {"code": code})[0] == 403, "pairing code reused")
            headers = client.json("/api/sessions")[2]
            assert_true(headers["Cache-Control"] == "no-store", headers)
            assert_true(
                client.json("/api/sessions", headers={"Host": "evil.example"})[0] == 403,
                "host bypass",
            )
            reused = run(project, env, "--web", "--web-port", "12345", binary=binary)
            assert_true(
                reused.returncode == 0 and client.origin in reused.stdout,
                reused.stdout + reused.stderr,
            )
            assert_true(process.poll() is None, "master replaced")
            session = client.create(project)
            peer = run(project, env, "-p", "Independent CLI session", binary=binary)
            assert_true(peer.returncode == 0 and "A real saved answer" in peer.stdout, peer.stderr)
            assert_true(len(provider.requests) == 1, provider.requests)
            client.command("submit", session, text="Inspect this session")
            request_id = f"{client.sequence:032x}"
            payload = dict(
                v=2,
                kind="submit",
                request_id=request_id,
                session_id=session["id"],
                generation=session["generation"],
                text="Inspect this session",
            )
            assert_true(
                client.json("/api/command", payload)[0] == 200, "duplicate not acknowledged"
            )
            value = client.until(
                session,
                lambda value: (
                    value["metadata"]["status"] == "idle"
                    and bool(value.get("state", {}).get("view", {}).get("nodes"))
                ),
            )
            assert_true(len(provider.requests) == 2, provider.requests)
            # One web conversation despite the repeated submit; the CLI run
            # is saved beside it.
            files = [path for path in session_files(home) if path.name.startswith("web-")]
            assert_true(len(files) == 1, session_files(home))
            data = json.loads(files[0].read_text().splitlines()[1])
            assert_true("display" in data, data.keys())
            assert_true("Supplied thinking" in json.dumps(value), value)
            assert_true(
                all("display" not in message for message in provider.requests[0][1]["messages"]),
                provider.requests,
            )
            assert_true(
                client.json("/api/command", {**payload, "text": "different"})[0] == 409,
                "request ID conflict accepted",
            )
            assert_true(
                client.json(
                    "/api/command", {**payload, "request_id": "f" * 32, "generation": "stale"}
                )[0]
                == 409,
                "stale generation accepted",
            )
            status, denied, _ = client.json(
                "/api/command",
                {
                    "v": 2,
                    "request_id": "d" * 32,
                    "kind": "submit",
                    "session_id": session["id"],
                    "generation": session["generation"],
                    "text": "/context",
                },
            )
            assert_true(status == 200 and denied["accepted"], denied)
            client.until(session, lambda value: value["metadata"]["status"] == "idle")
            status, denied, _ = client.json(
                "/api/command", {**payload, "request_id": "e" * 32, "text": "/fork"}
            )
            assert_true(status == 409 and "conversation controls" in denied["error"], denied)
            for index, bare in enumerate(("/model",)):
                status, denied, _ = client.json(
                    "/api/command",
                    {**payload, "request_id": f"b{index:031x}", "text": bare},
                )
                assert_true(status == 200 and denied["accepted"], denied)
                client.until(session, lambda value: value["metadata"]["status"] == "idle")
            assert_true(
                [path for path in session_files(home) if path.name.startswith("web-")] == files,
                "slash fork changed worker identity",
            )
            client.command("close", session)
            assert_true(
                "Supplied thinking" in json.dumps(client.snapshot(session)),
                "thinking lost on reopen",
            )
            catalogue = client.json("/api/sessions?refresh=1")[1]
            scheduled = catalogue["scheduled"]
            assert_true(
                isinstance(scheduled, dict)
                and scheduled["tasks"] == []
                and scheduled["runs"] == [],
                catalogue,
            )
            assert_true(
                any(item["title"] == "Inspect this session" for item in catalogue["sessions"]),
                catalogue,
            )
            client.command("logout")
            assert_true(client.json("/api/sessions")[0] == 401, "revoked cookie remained valid")
