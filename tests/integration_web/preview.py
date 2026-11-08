import http.client
import json

from integration_support import SMALL_PNG, Server, assert_true, event
from web_support import WebClient, web_host


def test_web_preview_serves_the_folder_sandboxed(root, home, *, binary):
    project = root / "designs"
    (project / "design").mkdir(parents=True)
    (project / "design/hero-a.html").write_text(
        "<!doctype html><html><head><title>A</title></head><body>hi</body></html>"
    )
    (project / "design/style.css").write_text("body{color:red}")
    (project / "design/pixel.png").write_bytes(SMALL_PNG)
    (project / "design/notes.md").write_text("# not served")
    (project / ".mcp.json").write_text("{}")
    (root / "outside.html").write_text("<p>outside</p>")
    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            port = client.port
            client.pair(code)
            session = client.create(project, activate=False)
            grant = f"/api/sessions/{session['id']}/preview"

            status, body, _ = WebClient(port).request(grant, {})
            assert_true(status == 401, (status, body))
            status, body, _ = client.request(grant, {})
            base = json.loads(body)["base"]
            assert_true(status == 200 and base.startswith("/preview/"), body)
            status, again, _ = client.request(grant, {})
            assert_true(json.loads(again)["base"] == base, again)

            # What a sandboxed page sends: no cookie, Origin "null".
            def page(path, origin="null", method="GET"):
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                connection.request(
                    method, base + path, headers={"Origin": origin} if origin else {}
                )
                response = connection.getresponse()
                return response.status, response.read(), dict(response.getheaders())

            status, html, headers = page("design/hero-a.html")
            assert_true(status == 200, (status, html))
            assert_true(headers["Content-Type"].startswith("text/html"), headers)
            assert_true(html.startswith(b"<!doctype html><script>"), html[:80])
            assert_true(b"uagentDesign" in html, html[:200])
            policy = headers["Content-Security-Policy"]
            assert_true(policy.startswith("sandbox allow-scripts"), policy)
            assert_true(f"default-src http://127.0.0.1:{port}{base};" in policy, policy)
            assert_true("'self'" not in policy and "connect-src" not in policy, policy)
            assert_true(headers["Cross-Origin-Resource-Policy"] == "cross-origin", headers)
            assert_true(headers["Access-Control-Allow-Origin"] == "*", headers)

            status, css, headers = page("design/style.css")
            assert_true(status == 200 and headers["Content-Type"] == "text/css", headers)
            status, png, headers = page("design/pixel.png", origin=None)
            assert_true(status == 200 and headers["Content-Type"] == "image/png", headers)

            for refused in (
                "design/notes.md",
                ".mcp.json",
                "design",
                "../outside.html",
                "design/missing.html",
            ):
                status, body, _ = page(refused)
                assert_true(status == 404, (refused, status, body))
            status, body, _ = page("design/hero-a.html", origin="https://evil.example")
            assert_true(status == 403, (status, body))
            status, body, _ = page("design/hero-a.html", method="POST")
            assert_true(status == 403, (status, body))
            unknown = "/preview/" + "0" * 48 + "/design/hero-a.html"
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            connection.request("GET", unknown, headers={"Origin": "null"})
            assert_true(connection.getresponse().status == 404, unknown)
            # The cookie's own API still refuses an opaque origin.
            status, body, _ = client.request(
                f"/api/sessions/{session['id']}/files?list=", headers={"Origin": "null"}
            )
            assert_true(status == 403, (status, body))
            # A picture needs the agent's browser, which this host has not.
            status, body, _ = client.request(
                f"/api/sessions/{session['id']}/files?png=design/hero-a.html&w=390&h=844"
            )
            assert_true(status == 503, (status, body))
            # Logging the device out ends its grants.
            client.command("logout")
            status, body, _ = page("design/hero-a.html")
            assert_true(status == 404, (status, body))
