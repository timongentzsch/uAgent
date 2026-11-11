import json
import subprocess
from urllib.parse import urlencode

from integration_support import SMALL_PNG, Server, assert_true, event
from web_support import WebClient, web_host


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
    (project / "pixel.png").write_bytes(SMALL_PNG)
    (project / "shot.avif").write_bytes(b"\0\0\0\x1cftypavif" + bytes(16))
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
                        "shot.avif",
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
                status == 200
                and value.pop("revision")
                and value == {"text": "print('hi')\n", "bytes": 12, "more": False},
                value,
            )
            status, value = answer(read="big.txt")
            text = value["text"]
            assert_true(status == 200 and value["more"] and value["bytes"] == 1_200_001, status)
            assert_true(len(text.encode()) <= 1 << 20 and "�" not in text, len(text))
            assert_true(answer(read="blob.bin")[0] == 415, "binary file read as text")
            assert_true(answer(read=".mcp.json")[0] == 404, "self-configuration was readable")

            status, body, headers = files(raw="pixel.png")
            assert_true(status == 200 and body == SMALL_PNG, status)
            assert_true(headers["Content-Type"] == "image/png", headers)
            assert_true("Content-Disposition" not in headers, headers)
            assert_true(
                headers["Content-Security-Policy"].startswith("default-src 'none'"), headers
            )
            # The app itself may play the folder's video, from its own address only.
            policy = client.request("/")[2]["Content-Security-Policy"]
            assert_true(
                policy.startswith("default-src 'none'") and "media-src 'self';" in policy, policy
            )
            status, _, headers = files(raw="shot.avif")
            assert_true(
                headers["Content-Type"] == "image/avif" and "Content-Disposition" not in headers,
                headers,
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


def test_web_files_listing_says_what_a_folder_s_pages_are(root, home, *, binary):
    project = root / "kinds"
    slide = '<meta name="artboard" content="1920x1080 slide">'
    screen = '<meta name="artboard" content="1440x900">'
    paper = '<meta name="print" content="flow a4">'
    pages = {
        "decks/q3/01-cover.html": slide,
        "decks/q3/02-plan.html": slide,
        "decks/q3/style.css": "",
        "design/onboarding/01-welcome.html": screen,
        "design/onboarding/notes.html": "<p>plain</p>",
        "design/system/tokens.css": ":root{}",
        "design/system/book.html": paper,
        "docs/memo.html": paper,
        "docs/slide.html": slide,
        "plain/a.html": "<p>a</p>",
    }
    for name, head in pages.items():
        (project / name).parent.mkdir(parents=True, exist_ok=True)
        (project / name).write_text(head)
    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project, activate=False)

            def listed(folder):
                status, body, _ = client.request(
                    f"/api/sessions/{session['id']}/files?{urlencode({'list': folder})}"
                )
                value = json.loads(body)
                assert_true(status == 200, value)
                kinds = {
                    entry["name"]: entry["kind"] for entry in value["entries"] if "kind" in entry
                }
                return value["kind"], kinds

            assert_true(
                listed("decks/q3") == ("deck", {"01-cover.html": "slide", "02-plan.html": "slide"}),
                listed("decks/q3"),
            )
            # One page with an artboard makes a design; other HTML is its page too.
            assert_true(
                listed("design/onboarding") == ("design", {"01-welcome.html": "page"}),
                listed("design/onboarding"),
            )
            assert_true(
                listed("design/system") == ("system", {"book.html": "doc"}),
                listed("design/system"),
            )
            # A document is its own; the slide beside it is a deck of one.
            assert_true(
                listed("docs") == ("deck", {"memo.html": "doc", "slide.html": "slide"}),
                listed("docs"),
            )
            assert_true(listed("plain") == ("", {}), listed("plain"))
            assert_true(listed("") == ("", {}), listed(""))


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


def test_web_files_show_what_changed_in_a_repository(root, home, *, binary):
    plain = root / "plain"
    plain.mkdir()
    fresh = root / "fresh"
    fresh.mkdir()
    (fresh / "first.txt").write_text("first\n")
    project = root / "repo"
    project.mkdir()
    marker = root / "marker"

    def git(folder, *args):
        subprocess.run(
            [
                "git",
                "-c",
                "user.name=T",
                "-c",
                "user.email=t@example.com",
                "-C",
                str(folder),
                *args,
            ],
            check=True,
            capture_output=True,
        )

    git(fresh, "init", "-q")
    git(project, "init", "-q")
    for name in ("edited.txt", "staged.txt", "gone.txt", "old name.txt", "same.txt"):
        (project / name).write_text(f"{name} one\n")
    (project / "blob.bin").write_bytes(b"\x00\x01")
    git(project, "add", ".")
    git(project, "commit", "-q", "-m", "first")
    # A repository must not run a program through its own configuration.
    git(project, "config", "diff.external", f"touch {marker}; true")
    git(project, "config", "diff.evil.textconv", f"touch {marker}; cat")
    (project / ".gitattributes").write_text("*.txt diff=evil\n")
    (project / "edited.txt").write_text("edited.txt two\n")
    (project / "staged.txt").write_text("staged.txt two\n")
    git(project, "add", "staged.txt")
    (project / "gone.txt").unlink()
    git(project, "mv", "old name.txt", "new name.txt")
    (project / "added.txt").write_text("added\n")
    (project / "blob.bin").write_bytes(b"\x00\x02")
    # A folder inside a larger repository shows only itself.
    outer = root / "outer"
    inner = outer / "inner"
    inner.mkdir(parents=True)
    git(outer, "init", "-q")
    (outer / "beside.txt").write_text("beside one\n")
    (inner / "within.txt").write_text("within one\n")
    git(outer, "add", ".")
    git(outer, "commit", "-q", "-m", "first")
    (outer / "beside.txt").write_text("beside two\n")
    (inner / "within.txt").write_text("within two\n")
    (project / "huge.txt").write_text("a line of text\n" * 150_000)
    # Search leaves out what git ignores, and finds what it would add.
    searched = root / "searched"
    for folder in ("src", "notes", "node_modules/x"):
        (searched / folder).mkdir(parents=True)
    for path in ("src/types.ts", "notes/types.ts", "node_modules/x/types.ts"):
        (searched / path).write_text("")
    (searched / ".gitignore").write_text("node_modules/\n")
    git(searched, "init", "-q")
    git(searched, "add", "src", ".gitignore")
    git(searched, "commit", "-q", "-m", "first")
    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)

            def answer(folder, **query):
                session = client.create(folder, activate=False)
                status, value, _ = client.json(
                    f"/api/sessions/{session['id']}/files?{urlencode(query)}"
                )
                return status, value

            status, value = answer(project, changes="")
            states = {change["path"]: change["state"] for change in value["changes"]}
            assert_true(
                status == 200
                and value["repository"]
                and not value["more"]
                and states
                == {
                    ".gitattributes": "A",
                    "added.txt": "A",
                    "blob.bin": "M",
                    "edited.txt": "M",
                    "gone.txt": "D",
                    "huge.txt": "A",
                    "new name.txt": "R",
                    "staged.txt": "M",
                },
                value,
            )
            diffs = {path: answer(project, diff=path)[1] for path in states}
            assert_true("+edited.txt two" in diffs["edited.txt"]["text"], diffs)
            assert_true("+staged.txt two" in diffs["staged.txt"]["text"], diffs)
            assert_true("-gone.txt one" in diffs["gone.txt"]["text"], diffs)
            assert_true("+added" in diffs["added.txt"]["text"], diffs)
            assert_true("+old name.txt one" in diffs["new name.txt"]["text"], diffs)
            assert_true("Binary files" in diffs["blob.bin"]["text"], diffs)
            assert_true(not diffs["edited.txt"]["more"], diffs)
            assert_true(not marker.exists(), "a configured program ran")
            for path in ("../secret", "same.txt", "missing", ""):
                assert_true(answer(project, diff=path)[0] == 404, path)

            # Too long to carry whole, it is cut rather than refused.
            assert_true(diffs["huge.txt"]["more"] and "+a line" in diffs["huge.txt"]["text"], "cut")

            status, value = answer(inner, changes="")
            assert_true(value["changes"] == [{"path": "within.txt", "state": "M"}], value)
            status, value = answer(inner, diff="within.txt")
            assert_true(status == 200 and "+within two" in value["text"], value)
            for path in ("../beside.txt", "beside.txt", "inner/within.txt"):
                assert_true(answer(inner, diff=path)[0] == 404, path)

            status, value = answer(searched, find="types.ts")
            assert_true(
                status == 200
                and value == {"paths": ["notes/types.ts", "src/types.ts"], "more": False},
                value,
            )

            status, value = answer(plain, changes="")
            assert_true(status == 200 and value["changes"] == [] and not value["repository"], value)
            status, value = answer(fresh, changes="")
            assert_true(value["changes"] == [{"path": "first.txt", "state": "A"}], value)
            status, value = answer(fresh, diff="first.txt")
            assert_true(status == 200 and "+first" in value["text"], value)


def test_web_files_save_a_person_s_edit_against_its_revision(root, home, *, binary):
    project = root / "files"
    (project / "docs").mkdir(parents=True)
    (project / "docs/memo.html").write_text("<p>old</p>")
    (project / "blob.bin").write_bytes(b"\x00\x01binary")
    elsewhere = root / "elsewhere.txt"
    elsewhere.write_text("outside")
    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project, activate=False)
            files = f"/api/sessions/{session['id']}/files"
            _, read, _ = client.json(files + "?" + urlencode({"read": "docs/memo.html"}))
            revision = read["revision"]
            assert_true(read["text"] == "<p>old</p>" and revision, read)

            def save(path, text, revision):
                return client.json(files, {"path": path, "text": text, "revision": revision})

            status, saved, _ = save("docs/memo.html", "<p>new</p>", revision)
            assert_true(status == 200 and saved["revision"] != revision, (status, saved))
            assert_true((project / "docs/memo.html").read_text() == "<p>new</p>", "not saved")
            # Saved against what it was read at: a second save on the old
            # revision, or after the agent wrote, is refused.
            status, refused, _ = save("docs/memo.html", "<p>lost</p>", revision)
            assert_true(status == 409 and "changed" in refused["error"], (status, refused))
            (project / "docs/memo.html").write_text("<p>agent</p>")
            status, refused, _ = save("docs/memo.html", "<p>lost</p>", saved["revision"])
            assert_true(status == 409, (status, refused))
            assert_true((project / "docs/memo.html").read_text() == "<p>agent</p>", "overwrote")
            # Only a text file that is there, inside the folder.
            for path in ("docs/new.html", "blob.bin", "../elsewhere.txt", str(elsewhere)):
                status, refused, _ = save(path, "x", "")
                assert_true(status == 404, (path, status, refused))
            assert_true(not (project / "docs/new.html").exists(), "made a file")
            assert_true(elsewhere.read_text() == "outside", "wrote outside")
            # With create, a new page and its folder, never over a file, never
            # anything but a page, never outside.

            def create(path):
                return client.json(files, {"path": path, "text": "<table>", "create": True})[:2]

            status, made = create("sheets/new/budget.html")
            assert_true(status == 200 and made["revision"], (status, made))
            assert_true((project / "sheets/new/budget.html").read_text() == "<table>", "not made")
            (root / "outside").mkdir()
            (project / "linked").symlink_to(root / "outside")
            for path, expected in (
                ("sheets/new/budget.html", 409),
                ("docs/memo.html", 409),
                ("sheets/data.csv", 404),
                ("../made.html", 404),
                ("linked/made.html", 404),
            ):
                status, refused = create(path)
                assert_true(status == expected, (path, status, refused))
            assert_true((project / "docs/memo.html").read_text() == "<p>agent</p>", "overwrote")
            assert_true(not (root / "made.html").exists(), "made a file outside")
            assert_true(not list((root / "outside").iterdir()), "wrote through a link")
            assert_true(
                WebClient(client.port).request(files, {"path": "docs/memo.html", "text": "x"})[0]
                == 401,
                "anonymous save",
            )


def test_web_files_reorder_copy_and_delete_a_folder_s_pages(root, home, *, binary):
    project = root / "moves"
    deck = project / "deck"
    deck.mkdir(parents=True)
    for name in ("01-a.html", "02-b.html", "03-c.html"):
        (deck / name).write_text(name[3])
    (deck / "style.css").write_text("css")
    (project / "outside.html").write_text("outside")
    (deck / "link.html").symlink_to(project / "outside.html")
    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(project, activate=False)
            files = f"/api/sessions/{session['id']}/files"

            def move(*moves, path="deck"):
                keys = ("from", "to", "copy")
                return client.json(
                    files,
                    {"path": path, "moves": [dict(zip(keys, m, strict=False)) for m in moves]},
                )[:2]

            def pages():
                return {p.name: p.read_text() for p in deck.glob("*.html") if not p.is_symlink()}

            # Two pages swap names; a copy takes the place its page leaves.
            status, moved = move(("01-a.html", "02-a.html"), ("02-b.html", "01-b.html"))
            assert_true(status == 200 and moved == {"moved": 2}, (status, moved))
            status, moved = move(
                ("02-a.html", "03-a.html", True), ("03-c.html", "04-c.html"), ("01-b.html", "")
            )
            assert_true(status == 200, (status, moved))
            before = {"02-a.html": "a", "03-a.html": "a", "04-c.html": "c"}
            assert_true(pages() == before, pages())
            # Refused whole: a name that is taken, a page that is not there,
            # anything but a page of this folder, a link, another folder.
            refused = {
                409: [(("02-a.html", "04-c.html"),), (("02-a.html", "02-a.html", True),)],
                404: [
                    (("02-a.html", "05-a.html"), ("09-x.html", "06-x.html")),
                    (("link.html", ""),),
                ],
                400: [
                    (("02-a.html", "../up.html"),),
                    (("02-a.html", "sub/up.html"),),
                    (("../outside.html", "in.html"),),
                    (("style.css", "02-z.html"),),
                    (("02-a.html", "notes.txt"),),
                    (("02-a.html", ".hidden.html"),),
                    (("02-a.html", "x.html"), ("02-a.html", "y.html")),
                    (("02-a.html", "x.html"), ("04-c.html", "x.html")),
                    (("02-a.html", "", True),),
                    (),
                ],
            }
            for expected, cases in refused.items():
                for case in cases:
                    status, answer = move(*case)
                    assert_true(status == expected, (case, status, answer))
            status, answer = move(("outside.html", "in.html"), path="..")
            assert_true(status == 404, (status, answer))
            assert_true(pages() == before, pages())
            assert_true(
                sorted(p.name for p in deck.iterdir())
                == sorted([*before, "link.html", "style.css"]),
                list(deck.iterdir()),
            )
            assert_true((project / "outside.html").read_text() == "outside", "moved outside")


def test_web_folder_picker_lists_directories_on_the_main(root, home, *, binary):
    projects = home / "Projects"
    (projects / "nested").mkdir(parents=True)
    (projects / "notes.txt").write_text("not a folder")
    with web_host(binary, root, home, "http://127.0.0.1:1") as (web, code, _, _):
        assert_true(web.request("/api/folders?path=~")[0] == 401, "folder listing was public")
        web.pair(code)
        status, listing, _ = web.json("/api/folders?" + urlencode({"path": str(projects)}))
        assert_true(status == 200 and listing["path"] == str(projects.resolve()), listing)
        assert_true([row["name"] for row in listing["entries"]] == ["nested"], listing)
        assert_true(listing["parent"] == str(home.resolve()), listing)
        protected = home / ".uagent/web"
        status, _, _ = web.json("/api/folders?" + urlencode({"path": str(protected)}))
        assert_true(status == 404, "protected application state was browsable")
