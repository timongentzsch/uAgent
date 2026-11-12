import http.client
import json
from urllib.parse import quote, urlencode

from integration_support import SMALL_PNG, Server, assert_true, event, tool_call
from web_support import WebClient, web_host


def test_web_preview_serves_the_folder_sandboxed(root, home, *, binary):
    project = root / "designs"
    (project / "design").mkdir(parents=True)
    (project / "design/hero-a.html").write_text(
        "<!doctype html><html><head><title>A</title></head><body>hi</body></html>"
    )
    (project / "design/style.css").write_text("body{color:red}")
    (project / "design/pixel.png").write_bytes(SMALL_PNG)
    (project / "design/shot.avif").write_bytes(b"\0\0\0\x1cftypavif" + bytes(16))
    (project / "design/clip.mp4").write_bytes(b"\0\0\0\x18ftypisom" + bytes(16))
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
            # Pictures and video, which the app itself draws on a canvas.
            for name, kind in (
                ("pixel.png", "image/png"),
                ("shot.avif", "image/avif"),
                ("clip.mp4", "video/mp4"),
            ):
                status, _, headers = page(f"design/{name}", origin=None)
                assert_true(status == 200 and headers["Content-Type"] == kind, (name, headers))

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
            # A deck prints through a page made from the folder's file names:
            # the slides keep their own sandbox, the page runs none of them.
            (project / 'design/02 "b".html').write_text("<p>b</p>")
            (project / "design/03-left-out.html").write_text(
                '<meta name="artboard" content="1440x900 slide"><meta name="slide" content="skip">'
            )
            status, deck, headers = page("design/?deck&w=1440&h=99999")
            assert_true(status == 200, (status, deck))
            assert_true(b"@page{size:1440px 8000px;margin:0}" in deck, deck[:200])
            assert_true(
                deck.index(b'<iframe src="02%20%22b%22.html">')
                < deck.index(b'<iframe src="hero-a.html">'),
                deck,
            )
            # A slide that is skipped is not printed.
            assert_true(b"left-out" not in deck, deck)
            policy = headers["Content-Security-Policy"]
            assert_true("sandbox" not in policy and "frame-ancestors 'none'" in policy, policy)
            assert_true(b"uagentDesign" not in deck, deck[:200])
            # A picture or a PDF needs the agent's browser, which this host has not.
            for asked in (
                "png=design/hero-a.html&w=390&h=844",
                "pdf=design/hero-a.html",
                "pdf=design/",
            ):
                status, body, _ = client.request(f"/api/sessions/{session['id']}/files?{asked}")
                assert_true(status == 503, (asked, status, body))
            # Logging the device out ends its grants.
            client.command("logout")
            status, body, _ = page("design/hero-a.html")
            assert_true(status == 404, (status, body))


def test_web_sheet_page_loads_its_engine_from_its_own_address(root, home, *, binary):
    project = root / "sheets-project"
    (project / "sheets").mkdir(parents=True)
    (project / "sheets/budget.html").write_text(
        '<!doctype html><meta name="sheet" content="1"><title>Budget</title>'
        '<table data-sheet="Costs"><tr><th>Item<tr><td>=1+1</table>'
        '<table data-sheet="Inputs"><tr><td>4.5%</table>'
    )
    (project / "sheets/book.xlsx").write_bytes(b"PK\x03\x04 workbook")
    (project / "sheets/rows.csv").write_text("a,b\n1,2\n")
    odd = 'Q1 100% #2 "é".csv'
    (project / "sheets" / odd).write_text("a,b\n1,2\n")
    (project / "~sheet").mkdir()
    (project / "~sheet/sheet.js").write_text("the folder's own")
    # The engine as a web build installs it, beside the helper the host inlines.
    dist = root / "dist"
    (dist / "share/sheet/snippets").mkdir(parents=True)
    (dist / "design-helper.js").write_text("/* uagentDesign */")
    (dist / "share/sheet/sheet.js").write_text("export const ready = 1;")
    (dist / "share/sheet/wasm_bg.wasm").write_bytes(b"\0asm")
    (dist / "share/sheet/snippets/inline0.js").write_text("export {};")
    (dist / "share/sheet/LICENSE-MIT").write_text("MIT")
    (dist / "secret.txt").write_text("not the engine")

    def responder(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "presented"})
        return tool_call("present", {"path": "sheets/budget.html"})

    with Server([responder]) as provider:
        engine = {"UAGENT_INTERNAL_WEB_DIST": str(dist)}
        with web_host(binary, root, home, provider.url, extra_env=engine) as (client, code, _, _):
            client.pair(code)
            session = client.create(project)
            port = client.port
            _, body, _ = client.request(f"/api/sessions/{session['id']}/preview", {})
            base = json.loads(body)["base"]

            def page(path):
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                connection.request("GET", base + path, headers={"Origin": "null"})
                response = connection.getresponse()
                return response.status, response.read(), dict(response.getheaders())

            status, _, headers = page("sheets/budget.html")
            policy = headers["Content-Security-Policy"]
            assert_true(status == 200 and "'wasm-unsafe-eval'" in policy, policy)
            for name, kind in (
                ("sheet.js", "text/javascript"),
                ("wasm_bg.wasm", "application/wasm"),
                ("snippets/inline0.js", "text/javascript"),
            ):
                status, got, headers = page(f"~sheet/{name}")
                assert_true(status == 200 and headers["Content-Type"] == kind, (name, headers))
                assert_true(headers["Access-Control-Allow-Origin"] == "*", headers)
            # The installed code answers, not a folder of that name; nothing
            # else of the install is reachable through it.
            assert_true(page("~sheet/sheet.js")[1] == b"export const ready = 1;", "shadowed")
            for refused in ("~sheet/LICENSE-MIT", "~sheet/../secret.txt", "~sheet/", "~sheet/x.js"):
                assert_true(page(refused)[0] == 404, refused)
            # An Excel or CSV file is served as itself, and on request as a
            # page the sheet code fills from it.
            xlsx = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
            for name, kind in (("book.xlsx", xlsx), ("rows.csv", "text/csv"), (odd, "text/csv")):
                # The page names its file as an address spells it.
                spelt = quote(name, safe="")
                status, got, headers = page(f"sheets/{spelt}")
                assert_true(status == 200 and headers["Content-Type"] == kind, (name, headers))
                status, shell, headers = page(f"sheets/{spelt}?sheet")
                assert_true(headers["Content-Type"].startswith("text/html"), headers)
                assert_true(
                    f'<meta name="sheet" content="{spelt}">'.encode() in shell
                    and b"uagentDesign" in shell
                    and b"workbook" not in shell,
                    shell[:300],
                )
            assert_true(page("sheets/none.xlsx?sheet")[0] == 404, "a file that is not there")
            assert_true(b"<table" in page("sheets/budget.html?sheet")[1], "a page stays itself")
            # A sheet is its own artifact, counted by its tabs.
            listing = client.json(
                f"/api/sessions/{session['id']}/files?{urlencode({'list': 'sheets'})}"
            )[1]
            entry = next(e for e in listing["entries"] if e["name"] == "budget.html")
            assert_true(
                (entry["kind"], entry["count"], listing["kind"]) == ("sheet", 2, ""), listing
            )
            client.command("permissions", session, mode="yolo")
            client.command("submit", session, text="Present the budget")
            value = client.until(
                session,
                lambda value: (
                    '"presented"' in json.dumps(value) and not value["metadata"]["turn_active"]
                ),
            )
            parts = [
                part
                for node in value["state"]["view"]["nodes"]
                for part in node.get("parts") or []
                if part["kind"] == "artifact"
            ]
            assert_true(
                parts
                == [
                    {
                        "kind": "artifact",
                        "type": "sheet",
                        "path": "sheets/budget.html",
                        "cover": "sheets/budget.html",
                        "count": 2,
                    }
                ],
                parts,
            )


def test_web_sheet_engine_is_found_beside_the_binary(root, home, *, binary):
    project = root / "installed"
    project.mkdir()
    (project / "budget.html").write_text('<!doctype html><meta name="sheet" content="1">')
    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project, activate=False)
            _, body, _ = client.request(f"/api/sessions/{session['id']}/preview", {})
            base = json.loads(body)["base"]
            for name in ("sheet.js", "sheet.css", "wasm.js", "wasm_bg.wasm"):
                connection = http.client.HTTPConnection("127.0.0.1", client.port, timeout=10)
                connection.request("GET", f"{base}~sheet/{name}", headers={"Origin": "null"})
                response = connection.getresponse()
                assert_true(response.status == 200 and len(response.read()) > 0, name)
