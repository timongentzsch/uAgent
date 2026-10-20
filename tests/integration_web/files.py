import json
from urllib.parse import urlencode

from integration_support import Server, assert_true, event
from web_support import WebClient, web_host

from integration_web._common import PNG


def test_web_files_list_read_serve_and_find(root, home, *, binary):
    project = root / "files"
    (project / "src").mkdir(parents=True)
    (project / "docs").mkdir()
    (project / "many").mkdir()
    (project / "src/main.py").write_text("print('hi')\n")
    (project / "b.txt").write_text("bee\n")
    (project / "A.md").write_text("# A\n")
    # A 2-byte character straddles the 1 MiB cut.
    (project / "big.txt").write_text("x" + "é" * 600_000)
    (project / "blob.bin").write_bytes(b"\x00\x01binary")
    (project / "pixel.png").write_bytes(PNG)
    (project / "draw.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg"></svg>')
    (project / "page.html").write_text("<script>alert(1)</script>")
    (project / ".mcp.json").write_text("{}")
    (project / "link").symlink_to(project / "src")
    for index in range(210):
        (project / f"many/f{index}.log").write_text("")
    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project, activate=False)

            def files(**query):
                return client.request(f"/api/sessions/{session['id']}/files?{urlencode(query)}")

            def answer(**query):
                status, body, _ = files(**query)
                return status, json.loads(body)

            status, value = answer(list="")
            # Folders first, each group by name whatever its case; the
            # self-configuration file is never shown.
            assert_true(status == 200 and not value["more"], value)
            assert_true(
                [(entry["name"], entry["dir"]) for entry in value["entries"]]
                == [("docs", True), ("link", True), ("many", True), ("src", True)]
                + [
                    (name, False)
                    for name in (
                        "A.md",
                        "b.txt",
                        "big.txt",
                        "blob.bin",
                        "draw.svg",
                        "page.html",
                        "pixel.png",
                    )
                ],
                value,
            )
            sizes = {entry["name"]: entry["size"] for entry in value["entries"]}
            assert_true(sizes["b.txt"] == 4 and sizes["src"] == 0, sizes)
            status, value = answer(list="src")
            assert_true(
                status == 200
                and value["entries"] == [{"name": "main.py", "dir": False, "size": 12}],
                value,
            )
            status, value = answer(read="link/main.py")
            assert_true(
                status == 200 and value == {"text": "print('hi')\n", "bytes": 12, "more": False},
                value,
            )
            status, value = answer(read="big.txt")
            text = value["text"]
            assert_true(status == 200 and value["more"] and value["bytes"] == 1_200_001, status)
            assert_true(len(text.encode()) <= 1 << 20 and "�" not in text, len(text))
            assert_true(answer(read="blob.bin")[0] == 415, "binary file read as text")
            assert_true(answer(read=".mcp.json")[0] == 404, "self-configuration was readable")

            status, body, headers = files(raw="pixel.png")
            assert_true(status == 200 and body == PNG, status)
            assert_true(headers["Content-Type"] == "image/png", headers)
            assert_true("Content-Disposition" not in headers, headers)
            assert_true(
                headers["Content-Security-Policy"].startswith("default-src 'none'"), headers
            )
            status, _, headers = files(raw="draw.svg")
            assert_true(headers["Content-Security-Policy"].startswith("sandbox;"), headers)
            status, body, headers = files(raw="page.html")
            assert_true(
                status == 200
                and headers["Content-Type"] == "application/octet-stream"
                and 'attachment; filename="page.html"' in headers["Content-Disposition"],
                headers,
            )
            status, _, headers = files(raw="pixel.png", download="1")
            assert_true("attachment" in headers.get("Content-Disposition", ""), headers)

            status, value = answer(find="s m p")
            assert_true(status == 200 and value == {"paths": ["src/main.py"], "more": False}, value)
            status, value = answer(find="LOG")
            assert_true(len(value["paths"]) == 200 and value["more"], value)
            assert_true(answer(list="", read="b.txt")[0] == 400, "two modes answered")
            assert_true(answer()[0] == 400, "no mode answered")


def test_web_files_stay_inside_the_folder(root, home, *, binary):
    project = root / "files"
    project.mkdir()
    elsewhere = root / "elsewhere"
    elsewhere.mkdir()
    (elsewhere / "secret.txt").write_text("secret")
    (project / "notes.txt").write_text("notes")
    (project / "outside").symlink_to(elsewhere)
    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            (project / "devices").symlink_to(home / ".uagent/web/devices.json")
            session = client.create(project, activate=False)
            endpoint = f"/api/sessions/{session['id']}/files?"
            status, value, _ = client.json(endpoint + urlencode({"list": ""}))
            assert_true(
                [entry["name"] for entry in value["entries"]] == ["notes.txt"],
                value,
            )
            for query in (
                {"read": "../elsewhere/secret.txt"},
                {"read": str(elsewhere / "secret.txt")},
                {"read": "outside/secret.txt"},
                {"list": "outside"},
                {"raw": "devices"},
                {"read": "missing.txt"},
            ):
                status, value, _ = client.json(endpoint + urlencode(query))
                assert_true(status == 404 and value == {"error": "not found"}, (query, value))
            status, value, _ = client.json(f"/api/sessions/{'0' * 32}/files?list=")
            assert_true(status == 404 and value["error"] == "unknown session", value)
            anonymous = WebClient(client.port)
            assert_true(
                anonymous.request(endpoint + urlencode({"read": "notes.txt"}))[0] == 401,
                "private file was public",
            )
