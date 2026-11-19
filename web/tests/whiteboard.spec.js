// A whiteboard opened in Files: its items laid where the file puts them,
// joined by arrows, and moved, resized and retyped there into the file.
import { readFile, writeFile } from "node:fs/promises";
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

// The board opened in Files, and its file as it then is.
async function openBoard(app, session, host, board = BOARD) {
  const path = "boards/signup/board.html";
  await writeFiles(host.project, { [path]: board });
  const files = await openFiles(app, session, "boards/signup/");
  return {
    files,
    stage: viewerOf(files).stage,
    saved: () => readFile(join(host.project, path), "utf8"),
    // As the agent would write it.
    rewrite: (text) => writeFile(join(host.project, path), text),
  };
}

test("a whiteboard draws its items and arrows, and a drag, the keys, the corner and a double click change the file; ⌘Z undoes, ⇧⌘Z redoes", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, saved } = await openBoard(app, session, host);
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
  // The page is drawn again from the file, and ⇧⌘Z makes the move again.
  await expect.poll(() => place("ad")).toEqual([60, 180]);
  await item("title").click();
  await app.keyboard.press("ControlOrMeta+Shift+z");
  await expect.poll(saved).toMatch(/id="ad"[^>]*data-at="(?!60 180)\d+ \d+"/);
  await expect.poll(() => place("ad")).not.toEqual([60, 180]);

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

test("arrows run in right-angled legs: straight between items that face each other, round one that stands between", async ({
  page: app,
  session,
  host,
}) => {
  const shape = (id, at) =>
    `<div id="${id}" data-shape="rect" data-at="${at}" data-size="160 80">${id}</div>`;
  const { stage } = await openBoard(
    app,
    session,
    host,
    `<!doctype html>
<meta charset="utf-8">
<meta name="artboard" content="1200x800">
<meta name="board">
<title>Routes</title>
${shape("a", "60 100")}${shape("between", "320 100")}${shape("b", "600 100")}${shape("low", "60 400")}
<hr data-from="a" data-to="between">
<hr data-from="a" data-to="b" data-label="past it">
<hr data-from="a" data-to="low">
`,
  );
  await expect(stage.locator("svg.arrows > path")).toHaveCount(3);
  // Each arrow's corners, turns included, and the box of the item between.
  const { routes, between } = await stage.locator("body").evaluate((body) => {
    const box = (id) => {
      const item = body.querySelector(`#${id}`);
      return [
        item.offsetLeft,
        item.offsetTop,
        item.offsetLeft + item.offsetWidth,
        item.offsetTop + item.offsetHeight,
      ];
    };
    return {
      between: box("between"),
      routes: [...body.querySelectorAll("svg.arrows > path")].map((path) => {
        const numbers = path
          .getAttribute("d")
          .match(/-?[\d.]+/g)
          .map(Number);
        return numbers.flatMap((x, at) =>
          at % 2 ? [] : [[x, numbers[at + 1]]],
        );
      }),
    };
  });
  for (const route of routes)
    for (const [at, [x, y]] of route.entries()) {
      if (!at) continue;
      const [fromX, fromY] = route[at - 1];
      expect(x === fromX || y === fromY, `${route}`).toBe(true);
    }
  const [facing, past, down] = routes;
  expect(facing).toEqual([
    [220, 140],
    [320, 140],
  ]);
  expect(down).toEqual([
    [140, 180],
    [140, 400],
  ]);
  // The one past the item between turns, and no leg of it runs through it.
  expect(past.length).toBeGreaterThan(2);
  const [left, top, right, bottom] = between;
  for (const [at, [x, y]] of past.entries()) {
    if (!at) continue;
    const [fromX, fromY] = past[at - 1];
    expect(
      Math.max(x, fromX) > left &&
        Math.min(x, fromX) < right &&
        Math.max(y, fromY) > top &&
        Math.min(y, fromY) < bottom,
      `${past}`,
    ).toBe(false);
  }
  // A shape is its fill: no line round it.
  expect(
    await stage
      .locator("#a")
      .evaluate((item) => getComputedStyle(item).borderTopStyle),
  ).toBe("none");
});

// Writes of the board's file wait until they are let go.
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
  const { files, stage } = await openBoard(app, session, host);
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

test("a whiteboard's change that was refused is not what ⌘Z undoes: the one before it is", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, saved } = await openBoard(app, session, host);
  await stage.locator("#n1").click();
  await app.keyboard.press("ArrowRight");
  await expect.poll(saved).toContain('data-at="801 100"');
  // The next write is refused, as when the agent wrote the file meanwhile.
  let refused = false;
  await app.route(`**/api/sessions/${session.id}/files`, (route) => {
    if (refused) return route.continue();
    refused = true;
    return route.fulfill({ status: 409, json: { error: "conflict" } });
  });
  await app.keyboard.press("ArrowRight");
  await expect(files.locator(".page-notice")).toContainText(
    "changed meanwhile",
  );
  expect(await saved()).toContain('data-at="801 100"');
  await expect(async () => {
    await stage.locator("#title").click();
    await app.keyboard.press("ControlOrMeta+z");
    expect(await saved()).toBe(BOARD);
  }).toPass();
});

test("a whiteboard the agent wrote meanwhile is not undone: ⌘Z is refused, said, and keeps nothing to undo", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, saved, rewrite } = await openBoard(app, session, host);
  await stage.locator("#n1").click();
  await app.keyboard.press("ArrowRight");
  await expect.poll(saved).toContain('data-at="801 100"');
  const undo = files.getByRole("button", { name: "Undo" });
  await expect(undo).toBeEnabled();
  const agents = (await saved()).replace("Which fields?", "Which fields now?");
  await rewrite(agents);
  await stage.locator("#title").click();
  await app.keyboard.press("ControlOrMeta+z");
  await expect(files.locator(".page-notice")).toHaveText(
    "Not undone: the file changed meanwhile.",
  );
  expect(await saved()).toBe(agents);
  await expect(undo).toBeDisabled();
});
