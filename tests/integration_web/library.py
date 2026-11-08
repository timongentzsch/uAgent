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
    app = root.resolve() / "app"
    (app / "design/system/components/Button").mkdir(parents=True)
    (app / "design/system/tokens.css").write_text(":root{--ink:#111}")
    (app / "design/system/components/Button/preview.html").write_text(
        '<!doctype html><meta name="artboard" content="400">'
    )
    (app / "report.html").write_text("<p>report</p>")

    def responder(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "shared"})
        return tool_call("artifact", {"path": "report.html"})

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
