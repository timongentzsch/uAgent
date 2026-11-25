// A whiteboard opened in Files: its items laid where the file puts them,
// joined by arrows, and changed by hand there into the file.
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

// The board opened in Files with Edit on (unless `edit` is false), and its
// file as it then is.
async function openBoard(app, session, host, board = BOARD, options = {}) {
  const path = "boards/signup/board.html";
  await writeFiles(host.project, { [path]: board });
  const files = await openFiles(app, session, "boards/signup/", options);
  const frame = files.locator(".page-stage iframe:not([inert])");
  await expect(
    viewerOf(files).stage.locator("[data-at]").first(),
  ).toBeVisible();
  if (options.edit !== false) {
    await files.getByRole("button", { name: "Edit" }).click();
    await expect(files.getByRole("group", { name: "Insert" })).toBeVisible();
  }
  return {
    files,
    stage: viewerOf(files).stage,
    // A point of the 1200 by 800 board, where it is in the window.
    at: async (x, y) => {
      const box = await frame.boundingBox();
      return [box.x + (x * box.width) / 1200, box.y + (y * box.height) / 800];
    },
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
  const { stage, saved, at } = await openBoard(app, session, host);
  const item = (id) => stage.locator(`#${id}`);
  // A press on the empty board, which gives the page the keys.
  const away = async () => app.mouse.click(...(await at(1100, 700)));
  await expect(item("ok")).toBeVisible();
  const place = (id) =>
    item(id).evaluate((element) => [element.offsetLeft, element.offsetTop]);
  expect(await place("ok")).toEqual([400, 160]);
  await expect(stage.locator("svg.arrows > path:not(.hit)")).toHaveCount(2);
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
    await away();
    await app.keyboard.press("ControlOrMeta+z");
    expect(await saved()).toBe(BOARD);
  }).toPass();
  // The page is drawn again from the file, and ⇧⌘Z makes the move again.
  await expect.poll(() => place("ad")).toEqual([60, 180]);
  await away();
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
  const corner = await stage.locator(".uagent-handle").nth(3).boundingBox();
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
  // And a shape's label, which keeps its shape.
  await item("ok").dblclick();
  await app.keyboard.press("ControlOrMeta+a");
  await app.keyboard.type("Valid now?");
  await item("title").click();
  await expect.poll(saved).toContain(">Valid now?</div>");
  await expect(item("ok").locator("svg.shape")).toHaveCount(1);
});

test("with Edit off a whiteboard is only looked at: a drag on an item moves nothing, and no tools show", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, saved } = await openBoard(app, session, host, BOARD, {
    edit: false,
  });
  const edit = files.getByRole("button", { name: "Edit" });
  await expect(edit).toHaveAttribute("aria-pressed", "false");
  const box = await stage.locator("#ad").boundingBox();
  await drag(app, [box.x + 20, box.y + 20], [box.x + 80, box.y + 60]);
  await stage.locator("#ad").click();
  await app.keyboard.press("Delete");
  await expect(stage.locator("[data-uagent-picked]")).toHaveCount(0);
  await expect(stage.locator(".uagent-handle")).toHaveCount(0);
  await expect(files.getByRole("group", { name: "Insert" })).toHaveCount(0);
  await expect(files.getByRole("toolbar", { name: "Picked" })).toHaveCount(0);
  expect(
    await stage
      .locator("#ad")
      .evaluate((item) => [item.offsetLeft, item.offsetTop]),
  ).toEqual([60, 180]);
  // Edit on and off again: the item picked then is let go with it.
  await edit.click();
  await expect(files.getByRole("group", { name: "Insert" })).toBeVisible();
  await stage.locator("#ad").click();
  await expect(files.getByRole("toolbar", { name: "Picked" })).toBeVisible();
  await edit.click();
  await expect(stage.locator("[data-uagent-picked]")).toHaveCount(0);
  await expect(files.getByRole("toolbar", { name: "Picked" })).toHaveCount(0);
  expect(await saved()).toBe(BOARD);
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
  await expect(stage.locator("svg.arrows > path:not(.hit)")).toHaveCount(3);
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
      routes: [...body.querySelectorAll("svg.arrows > path:not(.hit)")].map(
        (path) => {
          const numbers = path
            .getAttribute("d")
            .match(/-?[\d.]+/g)
            .map(Number);
          return numbers.flatMap((x, at) =>
            at % 2 ? [] : [[x, numbers[at + 1]]],
          );
        },
      ),
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
  const { files, stage, saved, at } = await openBoard(app, session, host);
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
    await app.mouse.click(...(await at(1100, 700)));
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

// Once the page has the tool the app armed: the next press is the tool's.
const armed = (stage) =>
  expect
    .poll(() => stage.locator("html").evaluate((root) => root.style.cursor))
    .toBe("crosshair");

// A press, a drag and a let go of the mouse, from one point to another.
async function drag(app, [x, y], [toX, toY]) {
  await app.mouse.move(x, y);
  await app.mouse.down();
  await app.mouse.move(toX, toY, { steps: 6 });
  await app.mouse.up();
}

test("an item put from the tools lands where the board is next pressed, in the file before the arrows, and Undo takes it away", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, saved, at } = await openBoard(app, session, host);
  await expect(stage.locator("#ok")).toBeVisible();
  const tools = files.getByRole("group", { name: "Insert" });
  await tools.getByRole("button", { name: "Note" }).click();
  await expect(tools.getByRole("button", { name: "Note" })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await armed(stage);
  await app.mouse.click(...(await at(1000, 600)));
  await expect
    .poll(saved)
    .toMatch(
      /Which fields\?<\/p>\n<p id="note1" class="note" data-fill="yellow" data-at="\d+0 \d+0"><\/p>\n<hr/,
    );
  await expect(stage.locator("#note1")).toHaveAttribute(
    "data-uagent-picked",
    "",
  );
  await expect(tools.getByRole("button", { name: "Select" })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  // A shape of the kind picked from its menu; Escape lets a tool go.
  await tools.getByRole("button", { name: "Note" }).click();
  await app.keyboard.press("Escape");
  await expect(tools.getByRole("button", { name: "Select" })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await tools.getByRole("button", { name: "Shape" }).click();
  await app.getByRole("menuitem", { name: "Cloud" }).click();
  await armed(stage);
  await app.mouse.click(...(await at(1000, 400)));
  await expect
    .poll(saved)
    .toMatch(
      /<div id="shape1" data-shape="cloud" data-at="\d+ \d+" data-size="160 80"><\/div>\n<hr/,
    );
  await expect(stage.locator("#shape1 svg.shape")).toHaveCount(1);
  const undo = files.getByRole("button", { name: "Undo" });
  await undo.click();
  await expect.poll(saved).not.toContain("shape1");
  await undo.click();
  await expect.poll(saved).toBe(BOARD);
});

test("items picked with Shift or a frame move together, line up and spread out, each as one change that one Undo takes back", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, saved, at } = await openBoard(app, session, host);
  const picked = stage.locator("[data-uagent-picked]");
  await stage.locator("#ad").click();
  await stage.locator("#n1").click({ modifiers: ["Shift"] });
  await expect(picked).toHaveCount(2);
  const bar = files.getByRole("toolbar", { name: "Picked" });
  await expect(bar.getByRole("button", { name: "Align" })).toBeVisible();
  await expect(bar.getByRole("button", { name: "Distribute" })).toHaveCount(0);
  await drag(app, await at(100, 200), await at(160, 260));
  await expect
    .poll(saved)
    .toMatch(/id="n1"[^>]*data-at="(?!800 100)\d+ \d+"[\s\S]*/);
  expect(await saved()).toMatch(/id="ad"[^>]*data-at="(?!60 180)\d+ \d+"/);
  const undo = files.getByRole("button", { name: "Undo" });
  await undo.click();
  await expect.poll(saved).toBe(BOARD);
  await expect(undo).toBeDisabled();

  // A frame drawn from the empty board picks what it touches, the diamond,
  // the database and the note, and a section only where it holds all of it.
  await drag(app, await at(1150, 790), await at(10, 90));
  await expect(picked).toHaveCount(5);
  await stage.locator("#ad").click({ modifiers: ["Shift"] });
  await expect(picked).toHaveCount(4);
  await bar.getByRole("button", { name: "Align" }).click();
  await app.getByRole("menuitemradio", { name: "Top" }).click();
  await expect
    .poll(saved)
    .toContain('id="db" data-shape="database" data-at="400 100"');
  for (const id of ["web", "ok", "n1"])
    expect(await saved()).toMatch(
      new RegExp(`id="${id}"[^>]*data-at="\\d+ 100"`),
    );
  await undo.click();
  await expect.poll(saved).toBe(BOARD);

  // Three spread out across: the first and the last stay.
  await stage.locator("#ad").click();
  await stage.locator("#ok").click({ modifiers: ["Shift"] });
  await stage.locator("#n1").click({ modifiers: ["Shift"] });
  await bar.getByRole("button", { name: "Distribute" }).click();
  await app.getByRole("menuitemradio", { name: "Across" }).click();
  // 60 to 960 holds 160 + 160 + 160 with 210 between each.
  await expect
    .poll(saved)
    .toContain('id="ok" data-shape="diamond" data-at="430 160"');
  expect(await saved()).toContain('id="ad" data-shape="pill" data-at="60 180"');
  await undo.click();
  await expect.poll(saved).toBe(BOARD);
});

test("a section is held by its name or its edge: a press inside it lets go of what is picked, and a frame drawn inside it picks what it touches, not the section", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, at } = await openBoard(app, session, host);
  const picked = stage.locator("[data-uagent-picked]");
  const inside = async () => app.mouse.click(...(await at(300, 350)));
  await stage.locator("#ad").click();
  await expect(picked).toHaveAttribute("id", "ad");
  await inside();
  await expect(picked).toHaveCount(0);
  await drag(app, await at(40, 150), await at(240, 260));
  await expect(picked).toHaveAttribute("id", "ad");
  await inside();
  await expect(picked).toHaveCount(0);
  await stage.locator("#web h2").click();
  await expect(picked).toHaveAttribute("id", "web");
  await inside();
  await expect(picked).toHaveCount(0);
  await app.mouse.click(...(await at(22, 250)));
  await expect(picked).toHaveAttribute("id", "web");
  // A frame round all of the section picks it with what it holds.
  await app.mouse.click(...(await at(1100, 700)));
  await drag(app, await at(10, 90), await at(740, 420));
  await expect(picked).toHaveCount(3);
});

test("the picked item's bar stands over it inside the stage, and changes its shape, colour, text size, order and copies, and deletes it with its arrows", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, saved } = await openBoard(app, session, host);
  const bar = files.getByRole("toolbar", { name: "Picked" });
  const inside = async (id, above) => {
    await stage.locator(`#${id}`).click();
    await expect(bar).toBeVisible();
    const [item, shown, room] = await Promise.all([
      stage.locator(`#${id}`).boundingBox(),
      bar.boundingBox(),
      files.locator(".page-stage").boundingBox(),
    ]);
    expect(
      above
        ? shown.y + shown.height <= item.y
        : shown.y >= item.y + item.height || shown.y + shown.height <= item.y,
    ).toBe(true);
    expect(shown.x).toBeGreaterThanOrEqual(room.x);
    expect(shown.y).toBeGreaterThanOrEqual(room.y);
    expect(shown.x + shown.width).toBeLessThanOrEqual(room.x + room.width);
    expect(shown.y + shown.height).toBeLessThanOrEqual(room.y + room.height);
  };
  await inside("db", true);
  // Where there is no room above the title, the bar goes under it.
  await inside("title", false);
  await bar.getByRole("button", { name: "Title" }).click();
  await app.getByRole("menuitemradio", { name: "Large" }).click();
  await expect
    .poll(saved)
    .toContain('<h2 id="title" data-at="40 30">Signup flow</h2>');

  await stage.locator("#ok").click();
  await bar.getByRole("button", { name: "Diamond" }).click();
  await app.getByRole("menuitemradio", { name: "Hexagon" }).click();
  await expect.poll(saved).toContain('id="ok" data-shape="hexagon"');
  await bar.getByRole("button", { name: "Grey" }).click();
  await app.getByRole("menuitemradio", { name: "Blue" }).click();
  await expect
    .poll(saved)
    .toContain('data-size="160 100" data-fill="blue">Valid?</div>');
  await bar.getByRole("button", { name: "Duplicate" }).click();
  await expect
    .poll(saved)
    .toContain(
      'Which fields?</p>\n<div id="ok1" data-shape="hexagon" data-at="420 180" data-size="160 100" data-fill="blue">Valid?</div>\n',
    );
  // The copy is picked, and goes again.
  await expect(stage.locator("#ok1")).toHaveAttribute("data-uagent-picked", "");
  await bar.getByRole("button", { name: "Delete" }).click();
  await expect.poll(saved).not.toContain('id="ok1"');

  await stage.locator("#ad").click();
  await bar.getByRole("button", { name: "Order" }).click();
  await app.getByRole("menuitem", { name: "Bring to front" }).click();
  await expect.poll(saved).toContain('Which fields?</p>\n<div id="ad"');
  await bar.getByRole("button", { name: "Order" }).click();
  await app.getByRole("menuitem", { name: "Send to back" }).click();
  await expect
    .poll(saved)
    .toContain('<title>Signup flow</title>\n<div id="ad"');

  // Deleted with the keys, the diamond takes both arrows with it.
  await stage.locator("#ok").click();
  await app.keyboard.press("Delete");
  await expect.poll(saved).not.toContain('id="ok"');
  expect(await saved()).not.toContain("<hr");
  await expect(stage.locator("svg.arrows > path:not(.hit)")).toHaveCount(0);
});

test("⌘D duplicates, ⌘C and ⌘V copy, and ⌘A picks every item", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, saved } = await openBoard(app, session, host);
  await stage.locator("#n1").click();
  await app.keyboard.press("ControlOrMeta+d");
  await expect
    .poll(saved)
    .toContain(
      '<p id="n2" class="note" data-fill="pink" data-at="820 120">Which fields?</p>',
    );
  await app.keyboard.press("ControlOrMeta+c");
  await app.keyboard.press("ControlOrMeta+v");
  await expect
    .poll(saved)
    .toContain('id="n3" class="note" data-fill="pink" data-at="840 140"');
  await app.keyboard.press("ControlOrMeta+a");
  await expect(stage.locator("[data-uagent-picked]")).toHaveCount(8);
  await app.keyboard.press("Escape");
  await expect(stage.locator("[data-uagent-picked]")).toHaveCount(0);
});

test("copies of items picked together keep the arrows between them, not those to the rest", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, saved } = await openBoard(app, session, host);
  await stage.locator("#ok").click();
  await stage.locator("#db").click({ modifiers: ["Shift"] });
  await app.keyboard.press("ControlOrMeta+d");
  await expect
    .poll(saved)
    .toMatch(
      /<div id="db1"[^\n]*\n<hr data-from="ad" data-to="ok">\n<hr data-from="ok" data-to="db" data-label="yes" data-dash>\n<hr data-from="ok1" data-to="db1" data-label="yes" data-dash>\n$/,
    );
  await expect(stage.locator("svg.arrows > path:not(.hit)")).toHaveCount(3);
});

test("an arrow drawn from a dot on the picked item's side to another is in the file; it is picked by its line and deleted", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, saved, at } = await openBoard(app, session, host);
  await stage.locator("#db").click();
  const dots = stage.locator(".uagent-dot");
  await expect(dots).toHaveCount(4);
  const side = await dots.nth(1).boundingBox();
  const to = await stage.locator("#n1").boundingBox();
  await drag(
    app,
    [side.x + side.width / 2, side.y + side.height / 2],
    [to.x + to.width / 2, to.y + to.height / 2],
  );
  await expect.poll(saved).toBe(`${BOARD}<hr data-from="db" data-to="n1">\n`);
  await expect(stage.locator("svg.arrows > path:not(.hit)")).toHaveCount(3);
  // A press on its line picks it; the bar has Delete alone.
  const middle = await stage
    .locator("svg.arrows > path.hit")
    .last()
    .evaluate((line) => {
      const point = line.getPointAtLength(line.getTotalLength() / 2);
      return [point.x, point.y];
    });
  await app.mouse.click(...(await at(...middle)));
  await expect(stage.locator("svg.arrows > path.picked")).toHaveCount(1);
  const bar = files.getByRole("toolbar", { name: "Picked" });
  await expect(bar.getByRole("button")).toHaveCount(1);
  await bar.getByRole("button", { name: "Delete" }).click();
  await expect.poll(saved).toBe(BOARD);
  // With the arrow tool, from any item to another; one let go on the empty
  // board makes none and keeps the tool, in the page and the app alike.
  const tools = files.getByRole("group", { name: "Insert" });
  await tools.getByRole("button", { name: "Arrow" }).click();
  await armed(stage);
  await drag(app, await at(100, 200), await at(1000, 700));
  await armed(stage);
  await expect(tools.getByRole("button", { name: "Arrow" })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  expect(await saved()).toBe(BOARD);
  await drag(app, await at(100, 200), await at(860, 160));
  await expect.poll(saved).toBe(`${BOARD}<hr data-from="ad" data-to="n1">\n`);
});

test("someone else's change keeps what is picked and lets the armed tool go, on the page and in the app alike", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, saved, rewrite, at } = await openBoard(
    app,
    session,
    host,
  );
  await stage.locator("#n1").click();
  const tools = files.getByRole("group", { name: "Insert" });
  await tools.getByRole("button", { name: "Note" }).click();
  await armed(stage);
  const changed = BOARD.replace("Signup flow</h1>", "Sign-up flow</h1>");
  await rewrite(changed);
  await expect(stage.locator("#title")).toHaveText("Sign-up flow");
  await expect(tools.getByRole("button", { name: "Select" })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await expect
    .poll(() => stage.locator("html").evaluate((root) => root.style.cursor))
    .toBe("");
  await expect(stage.locator("#n1")).toHaveAttribute("data-uagent-picked", "");
  await expect(files.getByRole("toolbar", { name: "Picked" })).toBeVisible();
  // The next press on the empty board lets the pick go and puts nothing.
  await app.mouse.click(...(await at(1000, 600)));
  await expect(files.getByRole("toolbar", { name: "Picked" })).toBeHidden();
  expect(await saved()).toBe(changed);
});

test("a page that only says it is a board changes nothing in its file", async ({
  page: app,
  session,
  host,
}) => {
  const path = "pages/plain.html";
  const plain = `<!doctype html>
<meta charset="utf-8">
<meta name="artboard" content="400x300">
<title>Plain</title>
<p id="a" data-at="1 1">A</p>
<script>
addEventListener("load", () => setTimeout(() => {
  const say = (message) => parent.postMessage({ uagentDesign: 1, ...message }, "*");
  say({ type: "ready", title: "Plain", artboard: { w: 400, h: 300 }, tweaks: null, height: 300, board: true });
  say({ type: "drawn" });
}, 300));
// With Edit on, a change.
addEventListener("message", (event) => {
  if (event.data?.type === "edit" && event.data.on)
    parent.postMessage({ uagentDesign: 1, type: "change", set: [{ id: "a", at: [50, 50] }] }, "*");
});
</script>
`;
  await writeFiles(host.project, { [path]: plain });
  const files = await openFiles(app, session, path);
  await files.getByRole("button", { name: "Edit" }).click();
  await expect(files.locator(".page-notice")).toContainText(
    "That change was not made",
  );
  expect(await readFile(join(host.project, path), "utf8")).toBe(plain);
});

test.describe("on a phone", () => {
  test.use({
    isMobile: true,
    hasTouch: true,
    viewport: { width: 390, height: 844 },
  });

  test("a whiteboard's zoom buttons give way to its tools, Fit is in the tools' menu; a tap picks an item and a drag moves it", async ({
    page: app,
    session,
    host,
  }) => {
    const { files, stage, saved } = await openBoard(app, session, host, BOARD, {
      phone: true,
    });
    await expect(stage.locator("#n1")).toBeVisible();
    await expect(files.getByRole("group", { name: "Insert" })).toBeVisible();
    await expect(files.getByRole("group", { name: "Zoom" })).toHaveCount(0);
    const box = await stage.locator("#n1").boundingBox();
    const [x, y] = [box.x + box.width / 2, box.y + box.height / 2];
    await app.touchscreen.tap(x, y);
    await expect(stage.locator("#n1")).toHaveAttribute(
      "data-uagent-picked",
      "",
    );
    await expect(files.getByRole("toolbar", { name: "Picked" })).toBeVisible();
    const cdp = await app.context().newCDPSession(app);
    const touch = (type, by) =>
      cdp.send("Input.dispatchTouchEvent", {
        type,
        touchPoints:
          type === "touchEnd" ? [] : [{ x: x + by, y: y + by, id: 1 }],
      });
    await touch("touchStart", 0);
    for (let by = 5; by <= 40; by += 5) await touch("touchMove", by);
    await touch("touchEnd", 40);
    await expect
      .poll(saved)
      .toMatch(/id="n1"[^>]*data-at="(?!800 100)\d+ \d+"/);
    // A tap again on the one picked retypes it.
    const moved = await stage.locator("#n1").boundingBox();
    await app.touchscreen.tap(
      moved.x + moved.width / 2,
      moved.y + moved.height / 2,
    );
    await expect(stage.locator("#n1")).toHaveAttribute(
      "contenteditable",
      "plaintext-only",
    );
    await files.getByRole("button", { name: "More tools" }).click();
    await expect(app.getByRole("menuitem", { name: "Fit" })).toBeVisible();
  });

  test("a second finger joining a drag puts the item back, writes nothing, and zooms", async ({
    page: app,
    session,
    host,
  }) => {
    const { files, stage, saved } = await openBoard(app, session, host, BOARD, {
      phone: true,
    });
    const box = await stage.locator("#n1").boundingBox();
    const [x, y] = [box.x + box.width / 2, box.y + box.height / 2];
    const at = () =>
      stage
        .locator("#n1")
        .evaluate((note) => [note.offsetLeft, note.offsetTop]);
    const level = () =>
      files
        .locator(".page-stage iframe:not([inert])")
        .evaluate((frame) => frame.getBoundingClientRect().width);
    const before = await level();
    const cdp = await app.context().newCDPSession(app);
    const touch = (type, points) =>
      cdp.send("Input.dispatchTouchEvent", {
        type,
        touchPoints: points.map(([px, py], id) => ({ x: px, y: py, id })),
      });
    await touch("touchStart", [[x, y]]);
    for (let by = 5; by <= 30; by += 5) await touch("touchMove", [[x + by, y]]);
    await expect.poll(at).not.toEqual([800, 100]);
    // The second finger lands, and the two spread apart.
    await touch("touchStart", [
      [x + 30, y],
      [x - 20, y + 20],
    ]);
    await expect.poll(at).toEqual([800, 100]);
    for (let by = 10; by <= 60; by += 10)
      await touch("touchMove", [
        [x + 30 + by, y],
        [x - 20 - by, y + 20 + by],
      ]);
    await touch("touchEnd", []);
    await expect.poll(level).toBeGreaterThan(before);
    expect(await at()).toEqual([800, 100]);
    // Looked at since: nothing was written.
    await app.waitForResponse((response) => response.url().includes("stamps="));
    expect(await saved()).toBe(BOARD);
  });
});
