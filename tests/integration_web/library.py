import json

from integration_support import Server, assert_true, event, tool_call
from web_support import web_host


def test_web_library_lists_everything_by_place(root, home, *, binary):
    site = root.resolve() / "site"
    (site / "design/landing").mkdir(parents=True)
    for name in ("hero-a", "hero-b"):
        (site / f"design/landing/{name}.html").write_text(
            f'<!doctype html><meta name="artboard" content="1440x900"><title>{name}</title>'
        )
    # Plain HTML is no design.
    (site / "index.html").write_text("<!doctype html><title>site</title>")
    # A folder of slides is a deck; a page on paper is a document of its own,
    # also beside slides; a system's book is the system's.
    (site / "decks/talk").mkdir(parents=True)
    for name in ("01-cover", "02-plan"):
        (site / f"decks/talk/{name}.html").write_text(
            f'<!doctype html><meta content="1920x1080 slide" name="artboard"><title>{name}</title>'
        )
    (site / "decks/talk/handout.html").write_text(
        '<!doctype html><meta name="print" content="flow a4"><title>handout</title>'
    )
    (site / "docs").mkdir()
    (site / "docs/memo.html").write_text(
        '<!doctype html><meta name="print" content="flow letter"><title>memo</title>'
    )
    app = root.resolve() / "app"
    (app / "design/system/components/Button").mkdir(parents=True)
    (app / "design/system/tokens.css").write_text(":root{--ink:#111}")
    (app / "design/system/components/Button/preview.html").write_text(
        '<!doctype html><meta name="artboard" content="400">'
    )
    (app / "design/system/book.html").write_text(
        '<!doctype html><meta name="print" content="flow a4"><title>book</title>'
    )
    (app / "report.html").write_text("<p>report</p>")

    def responder(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "shared"})
        return tool_call("present", {"path": "report.html"})

    with Server([responder]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            client.create(site, activate=False)
            shared = client.create(app)
            client.command("permissions", shared, mode="yolo")
            client.command("submit", shared, text="Share the report")
            client.until(
                shared,
                lambda value: (
                    "shared" in json.dumps(value) and not value["metadata"]["turn_active"]
                ),
            )
            # An upload is no shared file.
            status, _, _ = client.request(
                f"/api/sessions/{shared['id']}/attachments",
                raw=b"note",
                headers={"Content-Type": "text/plain"},
            )
            assert_true(status == 200, status)
            for place, key, text in (
                (site, "project/tone", "Plain sentences."),
                (app, "global/style", "---\ndescription: Minimal UI\n---\nbody"),
            ):
                client.command(
                    "memory", cwd=str(place), action="set", key=key, content=text, revision=""
                )

            status, body, _ = client.request("/api/library")
            assert_true(status == 200, (status, body))
            listing = json.loads(body)
            places = {place["path"]: place for place in listing["places"]}
            assert_true({str(site), str(app)} <= set(places), places)
            assert_true(places[str(app)]["session"] == shared["id"], places)

            def find(kind):
                return [
                    (item["name"], item["place"])
                    for item in listing["items"]
                    if item["kind"] == kind
                ]

            assert_true(find("design") == [("design/landing", str(site))], listing["items"])
            assert_true(find("system") == [("design/system", str(app))], listing["items"])
            assert_true(find("deck") == [("decks/talk", str(site))], listing["items"])
            assert_true(
                sorted(find("doc"))
                == [("decks/talk/handout.html", str(site)), ("docs/memo.html", str(site))],
                listing["items"],
            )
            deck = next(item for item in listing["items"] if item["kind"] == "deck")
            assert_true(deck["count"] == 2, deck)
            assert_true(find("file") == [("report.html", str(app))], listing["items"])
            memories = {item["name"]: item for item in listing["items"] if item["kind"] == "memory"}
            assert_true(memories["tone"]["place"] == str(site), memories)
            assert_true(memories["tone"]["description"] == "Plain sentences.", memories)
            # Saved for all, listed once, as no place's.
            assert_true(
                [item["place"] for item in listing["items"] if item["name"] == "style"] == [""],
                listing["items"],
            )
            assert_true(memories["style"]["description"] == "Minimal UI", memories)
            design = next(item for item in listing["items"] if item["kind"] == "design")
            assert_true(design["count"] == 2 and len(design["pages"]) == 2, design)
            assert_true(listing["limits"]["memory"] > 0, listing["limits"])

            # Deleting: only what the listing names, never a place or past it.
            def delete(**fields):
                return client.request("/api/library/delete", fields)[0]

            file = next(item for item in listing["items"] if item["kind"] == "file")
            for refused in (
                {"kind": "deck", "place": str(root), "key": deck["key"]},
                {"kind": "design", "place": str(site), "key": f"{site}/.."},
                {"kind": "file", "session": shared["id"], "key": "../" + file["key"]},
            ):
                assert_true(delete(**refused) == 404, refused)
            for fields in (
                {"kind": "deck", "place": str(site), "key": deck["key"]},
                {"kind": "doc", "place": str(site), "key": f"{site}/docs/memo.html"},
                {"kind": "file", "session": file["session"], "key": file["key"]},
            ):
                assert_true(delete(**fields) == 200, fields)
            for gone in ("decks/talk", "docs/memo.html"):
                assert_true(not (site / gone).exists(), gone)
            assert_true((site / "docs").is_dir() and (site / "index.html").exists(), "kept")
            listing = json.loads(client.request("/api/library")[1])
            kinds = sorted(item["kind"] for item in listing["items"] if item["kind"] != "memory")
            # The handout was the deck folder's: it went with it.
            assert_true(kinds == ["design", "system"], listing["items"])
