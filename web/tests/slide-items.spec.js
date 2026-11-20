// A deck's slide with Edit on: its elements are picked, moved, resized, put
// in, copied and deleted by hand, and typed in, each change into its file.
import { readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { test, expect, openFiles, viewerOf, writeFiles } from "./fixtures.js";

const STYLE = `html { font-size: 28px; }
body { margin: 0; width: 1920px; height: 1080px; font-family: sans-serif; }
main { padding: 128px; }
.cards { display: flex; gap: 40px; }
.card { flex: 1; padding: 40px; background: #eee; }
`;
const PLAN = `<!doctype html>
<html><head><meta charset="utf-8"><meta name="artboard" content="1920x1080 slide"><title>Plan</title><link rel="stylesheet" href="style.css"></head><body><main><h1>Plan</h1><div class="cards"><div class="card"><h2>One</h2><p>First step.</p></div><div class="card"><h2>Two</h2><p>Second step.</p></div></div><ul><li>Alpha</li><li>Beta</li></ul></main><aside>Say it.</aside></body></html>
`;
const NEXT = PLAN.replace("<title>Plan", "<title>Next").replace(
  "<h1>Plan",
  "<h1>Next",
);

// The deck opened at its first slide with Edit on, and that slide's file.
async function openSlide(app, session, host, plan = PLAN, options = {}) {
  const path = "deck/01-plan.html";
  await writeFiles(host.project, {
    "deck/style.css": STYLE,
    [path]: plan,
    "deck/02-next.html": NEXT,
    ...options.files,
  });
  const files = await openFiles(app, session, path, options);
  const { stage } = viewerOf(files);
  await expect(stage.locator("h1")).toBeVisible();
  if (options.edit !== false) {
    await files.getByRole("button", { name: "Edit" }).click();
    await expect(files.getByRole("group", { name: "Insert" })).toBeVisible();
  }
  const frame = files.locator(".page-stage iframe:not([inert])");
  return {
    files,
    stage,
    bar: files.getByRole("toolbar", { name: "Picked" }),
    tools: files.getByRole("group", { name: "Insert" }),
    // A point of the 1920 by 1080 slide, where it is in the window.
    at: async (x, y) => {
      const box = await frame.boundingBox();
      return [box.x + (x * box.width) / 1920, box.y + (y * box.height) / 1080];
    },
    saved: () => readFile(join(host.project, path), "utf8"),
    rewrite: (text) => writeFile(join(host.project, path), text),
  };
}
// A press, a drag and a let go of the mouse, from one point to another.
async function drag(app, [x, y], [toX, toY]) {
  await app.mouse.move(x, y);
  await app.mouse.down();
  await app.mouse.move(toX, toY, { steps: 6 });
  await app.mouse.up();
}
const middle = async (locator) => {
  const box = await locator.boundingBox();
  return [box.x + box.width / 2, box.y + box.height / 2];
};
const picked = (element) =>
  expect(element).toHaveAttribute("data-uagent-picked", "");
// Once the page has the tool the app armed: the next press is the tool's.
const armed = (stage, cursor = "crosshair") =>
  expect
    .poll(() => stage.locator("html").evaluate((root) => root.style.cursor))
    .toBe(cursor);

test("a press picks the outermost element, a drag moves it by its translate alone, a corner sizes it, Put back undoes the move alone; ⌘Z and ⇧⌘Z", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, bar, saved } = await openSlide(app, session, host);
  // A press in a list item picks the list; the bar has no container to
  // pick and nothing to put back.
  await stage.getByText("Beta").click();
  await picked(stage.locator("ul"));
  await expect(bar.getByRole("button", { name: "Duplicate" })).toBeVisible();
  await expect(
    bar.getByRole("button", { name: "Select container" }),
  ).toHaveCount(0);
  await expect(bar.getByRole("button", { name: "Put back" })).toHaveCount(0);
  const before = await stage
    .locator(".cards")
    .evaluate((cards) => cards.getBoundingClientRect().top);

  const from = await middle(stage.locator("h1"));
  await drag(app, from, [from[0] + 60, from[1] + 30]);
  await expect.poll(saved).toMatch(/<h1 style="translate: \d+px \d+px;">Plan/);
  expect((await saved()).replace(/ style="[^"]*"/, "")).toBe(PLAN);
  // Nothing else on the slide moved.
  expect(
    await stage
      .locator(".cards")
      .evaluate((cards) => cards.getBoundingClientRect().top),
  ).toBe(before);

  const undo = files.getByRole("button", { name: "Undo" });
  await undo.click();
  await expect.poll(saved).toBe(PLAN);
  await files.getByRole("button", { name: "Redo" }).click();
  await expect.poll(saved).toMatch(/<h1 style="translate: /);
  // Drawn again from the file, Edit still on.
  await expect(stage.locator("h1")).toHaveCSS("translate", /\d+px \d+px/);

  // A corner sets its width, and keeps its height to its text.
  await stage.getByText("Alpha").click();
  await picked(stage.locator("ul"));
  const corner = await middle(stage.locator(".uagent-handle").nth(3));
  await drag(app, corner, [corner[0] - 200, corner[1] + 50]);
  await expect.poll(saved).toMatch(/<ul style="width: \d+px;">/);
  // A size is undone with ⌘Z; Put back takes back a move, and leaves the
  // size, also one the author wrote.
  await expect(bar.getByRole("button", { name: "Put back" })).toHaveCount(0);
  const list = await middle(stage.getByText("Alpha"));
  await drag(app, list, [list[0] + 30, list[1]]);
  await expect.poll(saved).toMatch(/<ul style="width: \d+px; translate: /);
  await bar.getByRole("button", { name: "Put back" }).click();
  await expect.poll(saved).toMatch(/<ul style="width: \d+px;"><li>/);
  await expect(bar.getByRole("button", { name: "Put back" })).toHaveCount(0);
});

test("a press again on the one picked types in the text under it, which is saved; a press elsewhere ends the typing", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, saved, at } = await openSlide(app, session, host);
  const alpha = stage.getByText("Alpha");
  await alpha.click();
  await picked(stage.locator("ul"));
  await expect(alpha).not.toHaveAttribute("contenteditable");
  await alpha.click();
  await expect(alpha).toHaveAttribute("contenteditable", "plaintext-only");
  await app.keyboard.press("End");
  await app.keyboard.type(" one");
  await expect.poll(saved).toContain("<li>Alpha one</li>");
  await app.mouse.click(...(await at(1800, 1000)));
  await expect(stage.locator("[contenteditable]")).toHaveCount(0);
  await expect(stage.locator("[data-uagent-picked]")).toHaveCount(0);
  // A double click types at once.
  await stage.getByText("Second step.").dblclick();
  await expect(stage.getByText("Second step.")).toHaveAttribute(
    "contenteditable",
    "plaintext-only",
  );
});

test("each tool puts its kind where the slide is pressed, placed by hand; a text is typed in at once", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, tools, saved, at } = await openSlide(app, session, host);
  await tools.getByRole("button", { name: "Text" }).click();
  await armed(stage);
  await app.mouse.click(...(await at(1200, 800)));
  await expect
    .poll(saved)
    .toMatch(
      /<\/ul><p style="position: absolute; left: \d+px; top: \d+px; width: 480px;">Text<\/p><\/main>/,
    );
  const text = stage.locator("main > p");
  await picked(text);
  await expect(text).toHaveAttribute("contenteditable", "plaintext-only");
  await app.keyboard.type("Hello");
  await expect.poll(saved).toContain(">Hello</p></main>");
  // The text lies where the slide was pressed.
  const box = await text.evaluate((p) => p.getBoundingClientRect());
  expect(Math.abs(box.left - 1200)).toBeLessThanOrEqual(4);
  expect(Math.abs(box.top - 800)).toBeLessThanOrEqual(4);

  for (const [index, [name, kind]] of [
    ["Rectangle", "rect"],
    ["Rounded", "round"],
    ["Ellipse", "ellipse"],
  ].entries()) {
    await tools.getByRole("button", { name: "Shape" }).click();
    await app.getByRole("menuitem", { name }).click();
    await expect(tools.getByRole("button", { name: "Shape" })).toHaveAttribute(
      "aria-pressed",
      "true",
    );
    await armed(stage);
    await app.mouse.click(...(await at(300 + 400 * index, 700)));
    await expect
      .poll(saved)
      .toMatch(
        new RegExp(
          `<div data-shape="${kind}" style="background: var\\(--accent, currentcolor\\);[^"]* width: 240px; height: 160px;"></div></main>`,
        ),
      );
    await expect(tools.getByRole("button", { name: "Select" })).toHaveAttribute(
      "aria-pressed",
      "true",
    );
  }
  await tools.getByRole("button", { name: "Table" }).click();
  await armed(stage);
  await app.mouse.click(...(await at(900, 200)));
  await expect
    .poll(saved)
    .toMatch(/<table style="position: absolute;[^"]*"><thead><tr><th>Column/);
  // A shape put in by hand is laid over the others or under them.
  await stage.locator('[data-shape="rect"]').click();
  const bar = app.getByRole("toolbar", { name: "Picked" });
  await expect(bar.getByRole("button", { name: "Put back" })).toHaveCount(0);
  await bar.getByRole("button", { name: "Order" }).click();
  await app.getByRole("menuitem", { name: "Bring to front" }).click();
  await expect
    .poll(saved)
    .toMatch(/<\/table><div data-shape="rect"[^>]*><\/div><\/main>/);
});

test("a container is picked from what it holds, then moved, duplicated and deleted", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, bar, saved } = await openSlide(app, session, host);
  // A heading in a card is picked on its own.
  await stage.getByText("One", { exact: true }).click();
  await picked(stage.locator("h2").first());
  await bar.getByRole("button", { name: "Select container" }).click();
  const card = stage.locator(".card").first();
  await picked(card);
  // A press inside the picked container drags all of it.
  const from = await middle(stage.locator("h2").first());
  await drag(app, from, [from[0] - 40, from[1] - 20]);
  await expect
    .poll(saved)
    .toMatch(/<div class="card" style="translate: -\d+px -\d+px;"><h2>One/);

  await bar.getByRole("button", { name: "Duplicate" }).click();
  await expect
    .poll(saved)
    .toMatch(
      /First step\.<\/p><\/div><div class="card" style="position: absolute; left: -?\d+px; top: -?\d+px; width: \d+px;"><h2>One<\/h2>/,
    );
  await expect(stage.locator(".card")).toHaveCount(3);
  await picked(stage.locator(".card").nth(1));
  await bar.getByRole("button", { name: "Delete" }).click();
  await expect.poll(saved).not.toContain("position: absolute");
  await app.keyboard.press("Escape");
  await stage.getByText("Beta").click();
  await app.keyboard.press("Delete");
  await expect.poll(saved).not.toContain("<ul>");
});

test("a change the file does not take is said, and the slide is drawn again from the file", async ({
  page: app,
  session,
  host,
}) => {
  // The page fills the heading's value from a file: it shows other text
  // than the file has.
  const plan = PLAN.replace(
    "<h1>Plan</h1>",
    '<h1>Plan <span data-source="n.csv" data-field="n">0</span></h1>',
  );
  const { files, stage, saved } = await openSlide(app, session, host, plan, {
    files: { "deck/n.csv": "n\n7\n" },
  });
  await expect(stage.locator("h1")).toHaveText("Plan 7");
  await stage.locator("h1").click();
  await app.keyboard.press("ArrowRight");
  await expect(files.locator(".page-notice")).toContainText(
    "That change was not made",
  );
  await expect(stage.locator("h1")).toHaveCSS("translate", "none");
  expect(await saved()).toBe(plan);
});

test("someone else's change keeps what is picked and lets the armed tool go; a move whose write waits for it is kept", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, bar, tools, saved, rewrite } = await openSlide(
    app,
    session,
    host,
  );
  await stage.locator("h1").click();
  await tools.getByRole("button", { name: "Text" }).click();
  await armed(stage);
  const changed = PLAN.replace("<li>Beta", "<li>Gamma");
  await rewrite(changed);
  await expect(stage.getByText("Gamma")).toBeVisible();
  await expect(tools.getByRole("button", { name: "Select" })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await armed(stage, "");
  await picked(stage.locator("h1"));
  await expect(bar).toBeVisible();

  // The read the person's write starts with waits until let go, while
  // someone else changes the file.
  let release;
  const held = new Promise((resolve) => {
    release = resolve;
  });
  let first = true;
  await app.route(
    (url) => url.pathname.endsWith("/files") && url.searchParams.has("read"),
    async (route) => {
      if (first) {
        first = false;
        await held;
      }
      await route.continue();
    },
  );
  const reading = app.waitForRequest((request) =>
    request.url().includes("/files?read="),
  );
  await stage.getByText("Gamma").click();
  await app.keyboard.press("ArrowRight");
  await reading;
  const again = changed.replace("<h1>Plan", "<h1>Plans");
  await rewrite(again);
  for (let looks = 0; looks < 2; looks++)
    await app.waitForResponse((response) => response.url().includes("stamps="));
  release();
  await expect(stage.locator("h1")).toHaveText("Plans");
  await expect
    .poll(saved)
    .toBe(again.replace("<ul>", '<ul style="translate: 1px;">'));
  await expect(stage.locator("ul")).toHaveCSS("translate", "1px");
});

test("a run of arrow presses is written once the key is up, to the elements it moved, before a copy that follows; Escape and a step lose none", async ({
  page: app,
  session,
  host,
}) => {
  const shape = (left) =>
    `<div data-shape="rect" style="position: absolute; left: ${left}px; top: 700px; width: 100px; height: 100px;"></div>`;
  const plan = PLAN.replace(
    "</ul>",
    `</ul>${shape(200)}${shape(400)}${shape(600)}`,
  );
  const { stage, saved } = await openSlide(app, session, host, plan);
  const shapes = stage.locator("[data-shape]");
  await shapes.nth(0).click();
  await shapes.nth(1).click({ modifiers: ["Shift"] });
  await app.keyboard.press("ArrowRight");
  await app.keyboard.press("ControlOrMeta+d");
  await expect
    .poll(saved)
    .toMatch(/(<div data-shape="rect"[^>]*><\/div>){5}<\/main>/);
  const written = (await saved()).match(/<div data-shape="rect"[^>]*>/g);
  expect(written[0]).toContain("translate: 1px;");
  expect(written[2]).toContain("translate: 1px;");
  // The third was not moved; the copies are placed where they stand.
  expect(written[4]).toBe(
    '<div data-shape="rect" style="position: absolute; left: 600px; top: 700px; width: 100px; height: 100px;">',
  );
  // A nudge, Escape and a step to the next slide: the nudge is in this
  // slide's file, and the next slide's is as it was.
  await stage.locator("h1").click();
  await app.keyboard.press("ArrowDown");
  await app.keyboard.press("Escape");
  await app.keyboard.press("ArrowRight");
  await expect(stage.locator("h1")).toHaveText("Next");
  await expect.poll(saved).toMatch(/<h1 style="translate: 0px 1px;">Plan/);
  expect(await readFile(join(host.project, "deck/02-next.html"), "utf8")).toBe(
    NEXT,
  );
});

test("Edit turned off while the slide's code loads stays off", async ({
  page: app,
  session,
  host,
}) => {
  let release;
  const held = new Promise((resolve) => {
    release = resolve;
  });
  await app.route("**/~sheet/slide.js", async (route) => {
    await held;
    await route.continue();
  });
  const { files, stage } = await openSlide(app, session, host, PLAN, {
    edit: false,
  });
  const edit = files.getByRole("button", { name: "Edit" });
  const loading = app.waitForRequest("**/~sheet/slide.js");
  await edit.click();
  await loading;
  await edit.click();
  await expect(edit).toHaveAttribute("aria-pressed", "false");
  release();
  // Once the page has loaded it too, and gone on from where it waited.
  await stage
    .locator("body")
    .evaluate(() =>
      import(
        `${location.pathname.replace(/^(\/preview\/[^/]+\/).*$/, "$1")}~sheet/slide.js`
      ).then(() => {}),
    );
  await stage.locator("h1").click();
  await expect(stage.locator("[data-uagent-picked]")).toHaveCount(0);
  await expect(stage.locator(".uagent-handle")).toHaveCount(0);
});

test("typing done after a change waiting to be written follows it into the file, and typing before it goes first", async ({
  page: app,
  session,
  host,
}) => {
  const plan = PLAN.replace("<ul>", "<p>Gone</p><ul>");
  const { stage, saved } = await openSlide(app, session, host, plan);
  // The file's writes wait until let go.
  let release;
  const held = new Promise((resolve) => {
    release = resolve;
  });
  await app.route(`**/api/sessions/${session.id}/files`, async (route) => {
    if (route.request().method() === "POST") await held;
    await route.continue();
  });
  // A move, written first and held; then a paragraph deleted, which waits
  // behind it; then typing in a block after it.
  await stage.locator("h1").click();
  await app.keyboard.press("ArrowRight");
  await stage.getByText("Gone").click();
  await app.keyboard.press("Delete");
  await expect(stage.getByText("Gone")).toHaveCount(0);
  const beta = stage.getByText("Beta");
  await beta.click();
  await beta.click();
  await expect(beta).toHaveAttribute("contenteditable", "plaintext-only");
  await app.keyboard.press("End");
  await app.keyboard.insertText(" too");
  release();
  await expect.poll(saved).toContain("<li>Beta too</li>");
  const file = await saved();
  expect(file).not.toContain("Gone");
  expect(file).toContain('<h1 style="translate: 1px;">Plan');
});

test("a text made while a block is typed in leaves that block as it was, not editable", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, tools, saved } = await openSlide(app, session, host);
  const first = stage.getByText("First step.");
  await first.click();
  await first.click();
  await expect(first).toHaveAttribute("contenteditable", "plaintext-only");
  await tools.getByRole("button", { name: "Text" }).click();
  await armed(stage);
  await first.click();
  await expect.poll(saved).toContain(">Text</p></main>");
  await expect(first).not.toHaveAttribute("contenteditable");
  await expect(stage.locator("[contenteditable]")).toHaveCount(1);
});

test.describe("on a phone", () => {
  test.use({
    isMobile: true,
    hasTouch: true,
    viewport: { width: 390, height: 844 },
  });

  test("a tap picks, a second tap types, a drag moves and a corner sizes", async ({
    page: app,
    session,
    host,
  }) => {
    const { files, stage, saved } = await openSlide(app, session, host, PLAN, {
      phone: true,
    });
    const title = stage.locator("h1");
    const [x, y] = await middle(title);
    await app.touchscreen.tap(x, y);
    await picked(title);
    await expect(files.getByRole("toolbar", { name: "Picked" })).toBeVisible();
    await app.touchscreen.tap(x, y);
    await expect(title).toHaveAttribute("contenteditable", "plaintext-only");

    const cdp = await app.context().newCDPSession(app);
    const touch = (type, [px, py]) =>
      cdp.send("Input.dispatchTouchEvent", {
        type,
        touchPoints: type === "touchEnd" ? [] : [{ x: px, y: py, id: 1 }],
      });
    const swipe = async ([fromX, fromY], [byX, byY]) => {
      await touch("touchStart", [fromX, fromY]);
      for (let step = 1; step <= 5; step++)
        await touch("touchMove", [
          fromX + (byX * step) / 5,
          fromY + (byY * step) / 5,
        ]);
      await touch("touchEnd", [fromX + byX, fromY + byY]);
    };
    // A press on the list ends the typing and picks it; a drag across moves
    // it, and steps to no other slide.
    await app.touchscreen.tap(...(await middle(stage.getByText("Alpha"))));
    await picked(stage.locator("ul"));
    await expect(stage.locator("[contenteditable]")).toHaveCount(0);
    // Below the bar over it, which a finger near it would press.
    const from = await middle(stage.getByText("Beta"));
    await swipe(from, [80, 0]);
    await expect.poll(saved).toMatch(/<ul style="translate: \d+px[^"]*;">/);
    await expect(stage.locator("h1")).toHaveText("Plan");
    // The corner is drawn its size on screen at any zoom, and takes a finger
    // pressing beside it.
    const corner = stage.locator(".uagent-handle").nth(2);
    const drawn = await corner.boundingBox();
    expect(Math.round(drawn.width)).toBe(10);
    await swipe([drawn.x - 8, drawn.y + drawn.height + 8], [15, 15]);
    await expect
      .poll(saved)
      .toMatch(/<ul style="translate: [^"]*width: \d+px;">/);
  });
});
