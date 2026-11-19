// An open artifact follows its file, whoever changes it: the page takes the
// change where it stands, outlines what changed for a moment, and the pane
// shows while a call is writing it.
import { readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import {
  test,
  expect,
  artboard,
  openFiles,
  paper,
  selectProbeModel,
  viewerOf,
  writeFiles,
} from "./fixtures.js";

const BOARD = `<!doctype html>
<meta charset="utf-8">
<meta name="artboard" content="1200x800">
<meta name="board">
<title>Signup flow</title>
<h1 id="title" data-at="40 30">Signup flow</h1>
<div id="ad" data-shape="pill" data-at="60 180" data-size="160 60">Ad click</div>
<div id="ok" data-shape="diamond" data-at="400 160" data-size="160 100">Valid?</div>
<hr data-from="ad" data-to="ok">
`;
const MEMO = paper(
  "Memo",
  "<h1>Memo</h1><p>One</p><p>Two</p><p>Three</p><p>Four</p>",
);

// A file written into the project and opened in the pane, and a way to
// write it as someone else would, outside any call.
async function open(app, session, host, path, text) {
  await writeFiles(host.project, { [path]: text });
  const files = await openFiles(app, session, path);
  return {
    files,
    stage: viewerOf(files).stage,
    saved: () => readFile(join(host.project, path), "utf8"),
    rewrite: async (change) =>
      writeFile(
        join(host.project, path),
        change(await readFile(join(host.project, path), "utf8")),
      ),
  };
}
// The pane's next look at its folder.
const looked = (app) =>
  app.waitForResponse((response) => response.url().includes("stamps="));
// A value kept on the page's window, which a load would lose.
const mark = (stage) =>
  stage.locator("body").evaluate(() => {
    window.kept = true;
  });
const kept = (stage) => stage.locator("body").evaluate(() => window.kept);

test("a whiteboard rewritten on disk takes the change in place: the same page at its zoom, an item at its new place and a new one, both outlined a moment", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, rewrite } = await open(
    app,
    session,
    host,
    "boards/signup/board.html",
    BOARD,
  );
  await expect(stage.locator("#ad")).toBeVisible();
  const level = files.locator(".zoom-controls output");
  await files.getByRole("button", { name: "Zoom in" }).click();
  const zoomed = await level.textContent();
  await mark(stage);
  await looked(app);
  await rewrite((text) =>
    text
      .replace('data-at="60 180"', 'data-at="300 600"')
      .replace("<hr", '<p id="n2" class="note" data-at="900 400">New</p>\n<hr'),
  );
  await expect(stage.locator("#n2")).toBeVisible({ timeout: 2500 });
  await expect(stage.locator("#ad")).toHaveAttribute("data-uagent-changed");
  await expect(stage.locator("#n2")).toHaveAttribute("data-uagent-changed");
  await expect(stage.locator("#ok")).not.toHaveAttribute("data-uagent-changed");
  expect(
    await stage
      .locator("#ad")
      .evaluate((item) => [item.offsetLeft, item.offsetTop]),
  ).toEqual([300, 600]);
  expect(await kept(stage)).toBe(true);
  await expect(level).toHaveText(zoomed);
  // The outline goes after a moment.
  await expect(stage.locator("#ad")).not.toHaveAttribute(
    "data-uagent-changed",
    { timeout: 3000 },
  );
});

test("a document's block changed on disk changes where it stands, the page not loaded again", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, rewrite } = await open(
    app,
    session,
    host,
    "docs/memo.html",
    MEMO,
  );
  await expect(stage.getByText("Two")).toBeVisible();
  await mark(stage);
  await looked(app);
  await rewrite((text) => text.replace("<p>Two</p>", "<p>Two, rewritten</p>"));
  const block = stage.getByText("Two, rewritten");
  await expect(block).toBeVisible({ timeout: 2500 });
  await expect(block).toHaveAttribute("data-uagent-changed");
  await expect(stage.getByText("One")).not.toHaveAttribute(
    "data-uagent-changed",
  );
  expect(await kept(stage)).toBe(true);
});

test("a page with a script of its own loads again for a change", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, rewrite } = await open(
    app,
    session,
    host,
    "design/scripted.html",
    artboard("Scripted", "<h1>Before</h1><script>window.ran = true</script>"),
  );
  await expect(stage.getByRole("heading", { name: "Before" })).toBeVisible();
  await mark(stage);
  await looked(app);
  await rewrite((text) => text.replace("Before", "After"));
  await expect(stage.getByRole("heading", { name: "After" })).toBeVisible({
    timeout: 2500,
  });
  expect(await kept(stage)).toBeUndefined();
});

test("a file no page draws from changes nothing; a stylesheet loads the page again", async ({
  page: app,
  session,
  host,
}) => {
  const { stage } = await open(
    app,
    session,
    host,
    "design/home.html",
    artboard("Home", "<h1>Home</h1>"),
  );
  await expect(stage.locator("h1")).toBeVisible();
  await looked(app);
  await mark(stage);
  await writeFile(join(host.project, "design/build.log"), "one\n");
  await looked(app);
  await writeFile(join(host.project, "design/build.log"), "one\ntwo\n");
  await looked(app);
  await looked(app);
  expect(await kept(stage)).toBe(true);
  await writeFile(join(host.project, "design/look.css"), "h1 { margin: 0 }");
  await looked(app);
  await writeFile(join(host.project, "design/look.css"), "h1 { margin: 1px }");
  await expect.poll(() => kept(stage)).toBeUndefined();
});

test("a person's own change is not taken for someone else's: no update, no outline", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, saved } = await open(
    app,
    session,
    host,
    "boards/signup/board.html",
    BOARD,
  );
  const item = stage.locator("#ad");
  await expect(item).toBeVisible();
  await stage.locator("body").evaluate(() => {
    window.updates = 0;
    addEventListener("message", (event) => {
      if (event.data?.type === "update") window.updates++;
    });
  });
  const box = await item.boundingBox();
  await app.mouse.move(box.x + 20, box.y + 20);
  await app.mouse.down();
  await app.mouse.move(box.x + 60, box.y + 60, { steps: 5 });
  await app.mouse.up();
  await expect.poll(saved).toMatch(/id="ad"[^>]*data-at="(?!60 180)\d+ \d+"/);
  // Looked at twice since: its own write is all that changed.
  await looked(app);
  await looked(app);
  expect(await stage.locator("body").evaluate(() => window.updates)).toBe(0);
  await expect(stage.locator("[data-uagent-changed]")).toHaveCount(0);
});

test("a change that arrives while a block is typed in waits until the typing is written, and loses none of it", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, saved, rewrite } = await open(
    app,
    session,
    host,
    "docs/memo.html",
    MEMO,
  );
  await files.getByRole("button", { name: "Edit" }).click();
  const one = stage.getByText("One");
  await expect(one).toHaveAttribute("contenteditable", "plaintext-only");
  await one.click();
  await app.keyboard.press("End");
  // Typing pauses only as the test says.
  await app.clock.install();
  await app.clock.pauseAt((await app.evaluate(() => Date.now())) + 60_000);
  await app.keyboard.insertText(" typed");
  await rewrite((text) =>
    text.replace("<p>Four</p>", "<p>Four, rewritten</p>"),
  );
  // Seen, and held while what was typed is not in the file.
  await looked(app);
  await looked(app);
  await expect(stage.getByText("Four, rewritten")).toHaveCount(0);
  await app.clock.runFor(1000);
  await expect.poll(saved).toContain("<p>One typed</p>");
  await app.clock.resume();
  await expect(stage.getByText("Four, rewritten")).toBeVisible();
  await expect(stage.getByText("One typed")).toBeVisible();
  expect(await saved()).toContain("<p>Four, rewritten</p>");
});

test("while a call writes the open file the pane says so, and stops once it has", async ({
  page: app,
  session,
  host,
  command,
}) => {
  const { files, stage } = await open(
    app,
    session,
    host,
    "design/01-hero-a.html",
    artboard("Hero A", "<h1>Alpha</h1>"),
  );
  await expect(stage.getByText("Alpha")).toBeVisible();
  // Asked first, the call runs until it is allowed.
  await selectProbeModel(command, session);
  await app.locator("#prompt").fill("Design probe");
  await app.locator("#prompt").press("Enter");
  const writing = files.locator(".page-tools .chip.working");
  await expect(writing).toHaveText("Agent writing");
  await app.getByRole("button", { name: "Allow once", exact: true }).click();
  await expect(writing).toHaveCount(0);
  await expect(stage.getByRole("heading", { name: "Rewritten" })).toBeVisible();
});
