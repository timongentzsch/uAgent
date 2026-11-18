// A whiteboard opened in Files: its items laid where the file puts them,
// joined by arrows, and moved, resized and retyped there into the file.
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { test, expect, openFiles, viewerOf, writeFiles } from "./fixtures.js";

const BOARD = `<!doctype html>
<meta charset="utf-8">
<meta name="artboard" content="1200x800">
<meta name="board">
<title>Signup flow</title>
<h1 id="title" data-at="40 30">Signup flow</h1>
<section id="web" data-at="20 100" data-size="700 300"><h2>Web</h2></section>
<div id="ad" data-shape="pill" data-at="60 180" data-size="160 60">Ad click</div>
<div id="ok" data-shape="diamond" data-at="400 160" data-size="160 100">Valid?</div>
<div id="db" data-shape="database" data-at="400 500" data-size="160 100">Leads</div>
<p id="n1" class="note" data-fill="pink" data-at="800 100">Which fields?</p>
<hr data-from="ad" data-to="ok">
<hr data-from="ok" data-to="db" data-label="yes" data-dash>
`;

test("a whiteboard draws its items and arrows, and a drag, the keys, the corner and a double click change the file; ⌘Z undoes", async ({
  page: app,
  session,
  host,
}) => {
  const path = "boards/signup/board.html";
  await writeFiles(host.project, { [path]: BOARD });
  const saved = () => readFile(join(host.project, path), "utf8");
  const files = await openFiles(app, session, "boards/signup/");
  const { stage } = viewerOf(files);
  const item = (id) => stage.locator(`#${id}`);
  await expect(item("ok")).toBeVisible();
  const place = (id) =>
    item(id).evaluate((element) => [element.offsetLeft, element.offsetTop]);
  expect(await place("ok")).toEqual([400, 160]);
  await expect(stage.locator("svg.arrows > path")).toHaveCount(2);
  await expect(stage.locator("svg.arrows text")).toHaveText("yes");
  await expect(item("ok").locator("svg.shape")).toHaveCount(1);

  // A drag moves the item to the 10px grid, and the file says so.
  const box = await item("ad").boundingBox();
  await app.mouse.move(box.x + 20, box.y + 20);
  await app.mouse.down();
  await app.mouse.move(box.x + 60, box.y + 60, { steps: 5 });
  await app.mouse.up();
  await expect.poll(saved).toMatch(/id="ad"[^>]*data-at="(?!60 180)\d+ \d+"/);
  expect((await saved()).replace(/id="ad"[^>]*>/, "")).toBe(
    BOARD.replace(/id="ad"[^>]*>/, ""),
  );
  // ⌘Z puts it back, once the page drawn again from the file has its keys.
  await expect(async () => {
    await item("title").click();
    await app.keyboard.press("ControlOrMeta+z");
    expect(await saved()).toBe(BOARD);
  }).toPass();

  // The keys move the picked item a pixel at a time.
  await item("n1").click();
  await app.keyboard.press("ArrowRight");
  await app.keyboard.press("ArrowRight");
  await expect.poll(saved).toContain('data-at="802 100"');

  // The picked item's corner resizes it.
  await item("ok").click();
  const corner = await stage.locator(".uagent-handle").boundingBox();
  await app.mouse.move(corner.x + 3, corner.y + 3);
  await app.mouse.down();
  await app.mouse.move(corner.x + 40, corner.y + 30, { steps: 5 });
  await app.mouse.up();
  await expect
    .poll(saved)
    .toMatch(/id="ok"[^>]*data-size="(?!160 100)\d+ \d+"/);

  // A double click retypes a note.
  await item("n1").dblclick();
  await app.keyboard.press("ControlOrMeta+a");
  await app.keyboard.type("Email only");
  await item("title").click();
  await expect.poll(saved).toContain(">Email only</p>");
  if (process.env.BOARD_SHOT)
    await files
      .locator(".page-stage")
      .screenshot({ path: process.env.BOARD_SHOT });
});

// A write of the board's file waits until it is let go, or is refused once.
async function holdWrite(app, session) {
  let release;
  const held = new Promise((resolve) => {
    release = resolve;
  });
  await app.route(`**/api/sessions/${session.id}/files`, async (route) => {
    await held;
    await route.continue();
  });
  return release;
}

test("a whiteboard opened alone while its change is being written opens as the file then is", async ({
  page: app,
  session,
  host,
}) => {
  const path = "boards/signup/board.html";
  await writeFiles(host.project, { [path]: BOARD });
  const files = await openFiles(app, session, "boards/signup/");
  const { stage } = viewerOf(files);
  const release = await holdWrite(app, session);
  const writing = app.waitForRequest(
    (request) =>
      request.method() === "POST" && request.url().endsWith("/files"),
  );
  await stage.locator("#n1").click();
  await app.keyboard.press("ArrowRight");
  await writing;
  const [alone] = await Promise.all([
    app.waitForEvent("popup"),
    files.getByRole("button", { name: "Open alone" }).click(),
  ]);
  // Two frames on, the window still waits for the write.
  await app.evaluate(
    () =>
      new Promise((done) =>
        requestAnimationFrame(() => requestAnimationFrame(done)),
      ),
  );
  expect(alone.url()).toBe("about:blank");
  release();
  await alone.waitForURL(/\/preview\//);
  await expect(alone.locator("#n1")).toHaveAttribute("data-at", "801 100");
});
