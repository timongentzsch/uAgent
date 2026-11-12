// A design's pages open as a canvas in the Files pane, a deck's slides and a
// document's pages in a viewer with a legend, each page in a sandboxed frame.
import { readdir, rename, writeFile } from "node:fs/promises";
import { join } from "node:path";
import {
  test,
  expect,
  artboard,
  openFiles,
  openSession,
  paper,
  selectProbeModel,
  slide,
  viewerOf,
  writeFiles,
} from "./fixtures.js";

// A design of three pages, one of which throws as it loads.
const design = (project) =>
  writeFiles(join(project, "design"), {
    "01-hero-a.html": artboard("Hero A", "<h1>Alpha</h1>"),
    "01-hero-b.html": artboard("Hero B", "<h1>Beta</h1>"),
    // An error as the page would raise it, without failing the test's page.
    "cover.html": artboard(
      "Cover",
      '<p>Cover</p><script>dispatchEvent(new ErrorEvent("error", { message: "ReferenceError: missing is not defined", lineno: 3 }))</script>',
      "390x844",
    ),
  });

test("a design folder opens as a canvas of sandboxed pages", async ({
  page: app,
  session,
  host,
}) => {
  await design(host.project);
  const files = await openFiles(app, session, "design/");
  // Rows by step: the pages without a number first, then step 01.
  await expect(files.getByText("01 · Hero A")).toBeVisible();
  await expect(
    files.getByRole("button", { name: "Open Hero A" }),
  ).toBeVisible();
  const alpha = app.frameLocator('iframe[title="Hero A"]');
  await expect(alpha.getByRole("heading", { name: "Alpha" })).toBeVisible();
  // Sandboxed in an opaque origin: no cookie, no API.
  const reach = await app.frame({ url: /hero-a\.html/ }).evaluate(async () => {
    let cookie;
    try {
      cookie = document.cookie;
    } catch {
      cookie = "refused";
    }
    const api = await fetch("/api/sessions").then(
      (response) => response.status,
      () => "refused",
    );
    return { cookie, api };
  });
  expect(reach.cookie).toBe("refused");
  expect(reach.api).toBe("refused");
  // A page that throws is marked, and its error goes to the draft.
  await expect(files.getByText("1 page failed to run")).toBeVisible();
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    /`design\/cover\.html` threw: ReferenceError: missing is not defined \(line 3\)/,
  );
  // Opening a page shows it alone; Source shows its code; Back returns.
  await files.getByRole("button", { name: "Open Hero B" }).click();
  await expect(
    app.frameLocator('iframe[title="Hero B"]').getByText("Beta"),
  ).toBeVisible();
  await files.getByRole("button", { name: "Source" }).click();
  await expect(files.locator(".code-view")).toContainText("<h1>Beta</h1>");
  await files.getByRole("button", { name: "Source" }).click();
  await files.getByRole("button", { name: "Back to the canvas" }).click();
  await expect(
    files.getByRole("button", { name: "Open Hero A" }),
  ).toBeVisible();
});

test("an agent's write reloads its page on the canvas", async ({
  page: app,
  session,
  host,
  command,
}) => {
  await design(host.project);
  await openFiles(app, session, "design/");
  await expect(
    app.frameLocator('iframe[title="Hero A"]').getByText("Alpha"),
  ).toBeVisible();
  await selectProbeModel(command, session, { yolo: true });
  await app.locator("#prompt").fill("Design probe");
  await app.locator("#prompt").press("Enter");
  await expect(
    app.frameLocator('iframe[title="Hero A"]').getByText("Rewritten"),
  ).toBeVisible();
});

test("the canvas pans and zooms like a plane", async ({
  page: app,
  session,
  host,
}) => {
  await design(host.project);
  const files = await openFiles(app, session, "design/");
  const level = files.locator(".zoom-controls output");
  const plane = files.locator(".canvas .zoom-content");
  await expect(
    app.frameLocator('iframe[title="Hero A"]').getByText("Alpha"),
  ).toBeVisible();
  const before = await level.textContent();
  // A trackpad pinch arrives as the wheel with Ctrl.
  const stage = files.locator(".canvas .zoom-surface");
  const box = await stage.boundingBox();
  await app.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await app.keyboard.down("Control");
  await app.mouse.wheel(0, -200);
  await app.keyboard.up("Control");
  await expect(level).not.toHaveText(before);
  // A drag pans, and opens nothing.
  const moved = await plane.getAttribute("style");
  await app.mouse.down();
  await app.mouse.move(
    box.x + box.width / 2 + 120,
    box.y + box.height / 2 + 40,
    { steps: 5 },
  );
  await app.mouse.up();
  await expect(plane).not.toHaveAttribute("style", moved);
  await expect(files.locator(".canvas-page")).toHaveCount(3);
  await files.getByRole("button", { name: "Fit" }).click();
  await expect(
    files.getByRole("button", { name: "Open Hero A" }),
  ).toBeInViewport();
});

// The Files sheet's canvas, its stage, and the zoom level it shows.
async function phoneCanvas(app, session) {
  const files = await openFiles(app, session, "design/", { phone: true });
  await expect(
    app.frameLocator('iframe[title="Hero A"]').getByText("Alpha"),
  ).toBeVisible();
  const level = files.locator(".zoom-controls output");
  const box = await files.locator(".canvas .zoom-surface").boundingBox();
  return {
    files,
    stage: files.locator(".canvas .zoom-surface"),
    cx: box.x + box.width / 2,
    cy: box.y + box.height / 2,
    percent: async () => Number.parseInt(await level.textContent(), 10),
  };
}

test("one finger pans and two pinch the canvas in a phone's sheet, never pulling it closed", async ({
  browser,
  browserName,
  storageState,
  session,
  host,
}) => {
  test.skip(browserName !== "chromium", "touch is driven through CDP");
  await design(host.project);
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
    storageState,
  });
  try {
    const app = await context.newPage();
    const { files, cx, cy, percent } = await phoneCanvas(app, session);
    const cdp = await context.newCDPSession(app);
    // The fingers spread, the lower one moving down as a pull would.
    const touch = (type, spread) =>
      cdp.send("Input.dispatchTouchEvent", {
        type,
        touchPoints:
          type === "touchEnd"
            ? []
            : [
                { x: cx, y: cy - 20, id: 1 },
                { x: cx, y: cy + 20 + spread, id: 2 },
              ],
      });
    // One finger dragging down pans the plane; the sheet stays put.
    const plane = files.locator(".canvas .zoom-content");
    const placed = await plane.getAttribute("style");
    const one = (type, y) =>
      cdp.send("Input.dispatchTouchEvent", {
        type,
        touchPoints: type === "touchEnd" ? [] : [{ x: cx, y, id: 1 }],
      });
    await one("touchStart", cy - 100);
    for (let y = cy - 90; y <= cy + 100; y += 10) await one("touchMove", y);
    await expect(files).not.toHaveAttribute("data-dragging", "");
    await one("touchEnd", cy + 100);
    await expect(files).toBeVisible();
    await expect(plane).not.toHaveAttribute("style", placed);
    const before = await percent();
    await touch("touchStart", 0);
    for (let spread = 10; spread <= 160; spread += 10)
      await touch("touchMove", spread);
    await touch("touchEnd", 160);
    expect(await percent()).toBeGreaterThan(before * 2);
    await expect(files).toBeVisible();
    await expect(files).not.toHaveAttribute("data-dragging", "");
  } finally {
    await context.close();
  }
});

test("a trackpad pinch as Safari sends it zooms the canvas", async ({
  page: app,
  session,
  host,
}) => {
  await design(host.project);
  const { stage, cx, cy, percent } = await phoneCanvas(app, session);
  const before = await percent();
  await stage.evaluate(
    (node, [x, y]) => {
      for (const [type, scale] of [
        ["gesturestart", 1],
        ["gesturechange", 1.5],
        ["gesturechange", 2],
      ])
        node.dispatchEvent(
          Object.assign(new Event(type, { bubbles: true, cancelable: true }), {
            scale,
            clientX: x,
            clientY: y,
          }),
        );
    },
    [cx, cy],
  );
  await expect.poll(percent).toBeGreaterThanOrEqual(before * 2 - 1);
});

test("a page opened: select and comment, tweaks, print", async ({
  page: app,
  session,
  host,
}) => {
  const tweaks = JSON.stringify({
    "--gap": { editor: "range", default: 8, min: 0, max: 32, unit: "px" },
    variant: {
      editor: "enum",
      default: "split",
      options: ["split", "stacked"],
    },
  });
  await writeFiles(join(host.project, "design"), {
    "hero-a.html": artboard(
      "Hero A",
      `<script type="application/json" id="tweaks">${tweaks}</script><main><h1>Alpha headline</h1></main>`,
    ),
  });
  // A design of one page has no canvas: Open shows the page, with no legend.
  const files = await openFiles(app, session, "design/");
  const frame = app.frameLocator('iframe[title="Hero A"]');
  await expect(frame.getByText("Alpha headline")).toBeVisible();
  await expect(files.locator(".canvas-page")).toHaveCount(0);
  await expect(files.getByRole("listbox")).toHaveCount(0);

  // Select picks an element; the comment goes into the draft, pane open.
  await files.getByRole("button", { name: "Select" }).click();
  await frame.getByText("Alpha headline").click();
  // Its mark is the inverse of the page under it, seen on any page.
  await expect(files.locator(".page-mark.picked")).toHaveCSS(
    "mix-blend-mode",
    "difference",
  );
  await files.getByRole("textbox", { name: "Comment" }).fill("Make it bigger");
  await files.getByRole("button", { name: "Add to chat" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    "`design/hero-a.html` `main > h1` “Alpha headline” — Make it bigger",
  );
  await expect(files).toBeVisible();

  // Tweaks apply at once; Keep adds them for the agent.
  await files.getByRole("button", { name: "Tweaks" }).click();
  await files.getByRole("button", { name: "stacked" }).click();
  await expect(frame.locator("html")).toHaveAttribute(
    "data-variant",
    "stacked",
  );
  await files.getByRole("button", { name: "Keep" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    /`design\/hero-a\.html` tweaks: variant stacked$/,
  );

  // Export: pictures need the agent's browser, which this host has not, so
  // PDF opens the page alone, which prints itself.
  await files.getByRole("button", { name: "Export" }).click();
  await expect(app.getByRole("menuitem", { name: "PNG image" })).toHaveCount(0);
  await expect(app.getByRole("menuitem", { name: "Markdown" })).toHaveCount(0);
  const [alone] = await Promise.all([
    app.waitForEvent("popup"),
    app.getByRole("menuitem", { name: "Print or save as PDF" }).click(),
  ]);
  expect(alone.url()).toMatch(
    /\/preview\/[0-9a-f]{48}\/design\/hero-a\.html\?v=[\d.]+#print$/,
  );
  await alone.close();
});

test("a design system opens on its brand book, and its themes are under Tweaks", async ({
  page: app,
  session,
  host,
}) => {
  const linked = (sheet) => `<link rel="stylesheet" href="${sheet}">`;
  await writeFiles(join(host.project, "design"), {
    "system/tokens.css":
      ':root, [data-theme="light"] {\n  --page: #ffffff;\n}\n[data-theme="dark"] {\n  --page: #000000;\n}\n',
    // The book the agent writes: on A4, as tall as its pages.
    "system/book.html": paper(
      "Brand book",
      '<section style="height:297mm;background:var(--page)"><h1>Brand</h1></section>',
      "a4",
      linked("tokens.css"),
    ),
    "landing/hero-a.html": artboard(
      "Hero A",
      `${linked("../system/tokens.css")}<h1>Alpha</h1>`,
    ),
  });
  // Open on a design system is its book, never a canvas.
  const files = await openFiles(app, session, "design/system/");
  await expect(
    app
      .frameLocator('iframe[title="Brand book"]')
      .getByRole("heading", { name: "Brand" }),
  ).toBeVisible();
  // An A4 page fits the pane across, as a PDF does, and scrolls inside.
  const across = () =>
    files.locator(".page-stage").evaluate((stage) => ({
      stage: stage.getBoundingClientRect().width,
      page: stage.querySelector("iframe").getBoundingClientRect().width,
    }));
  await expect
    .poll(async () => (await across()).page)
    .toBeLessThanOrEqual((await across()).stage);
  await expect(files.getByRole("button", { name: "Export" })).toBeVisible();

  await files.getByRole("button", { name: "Back to the list" }).click();
  await files.getByRole("button", { name: "landing", exact: true }).click();
  await files.getByRole("button", { name: "hero-a.html", exact: true }).click();
  await files.getByRole("button", { name: "Tweaks" }).click();
  await files.getByRole("button", { name: "dark" }).click();
  const frame = app.frameLocator('iframe[title="Hero A"]');
  await expect(frame.locator("html")).toHaveAttribute("data-theme", "dark");
  await files.getByRole("button", { name: "Keep theme" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    "`design/system/tokens.css` theme dark: ",
  );
});

test("on a phone a book opens as one page: its tools one row, Back to its list", async ({
  page: app,
  session,
  host,
}) => {
  await writeFiles(join(host.project, "design/system"), {
    "tokens.css": ":root{--page:#fff}",
    "book.html": artboard(
      "Brand book",
      '<section style="height:297mm"><h1>Brand</h1></section>',
      "794",
    ),
  });
  const files = await openFiles(app, session, "design/system/book.html", {
    phone: true,
  });
  await expect(
    app
      .frameLocator('iframe[title="Brand book"]')
      .getByRole("heading", { name: "Brand" }),
  ).toBeVisible();
  // The page's tools stay one row, scrolling sideways when they run out.
  const tools = files.locator(".page-tools");
  const row = await tools.evaluate((bar) => {
    const tops = [...bar.children].map((child) => child.offsetTop);
    return new Set(tops).size;
  });
  expect(row).toBe(1);
  // Back leaves the page for the folder's list, never an empty view.
  await files.getByRole("button", { name: "Back to the list" }).click();
  await expect(files.getByText("Choose a file to read it here.")).toBeHidden();
  await expect(
    files.getByRole("button", { name: "book.html", exact: true }),
  ).toBeVisible();
});

test("Back closes the open page and shows the view it was opened from", async ({
  page: app,
  session,
  host,
}) => {
  await design(host.project);
  const files = await openFiles(app, session, "design");
  // From the list: back to the list, never to a canvas.
  await files
    .getByRole("button", { name: "01-hero-a.html", exact: true })
    .click();
  await expect(
    app.frameLocator('iframe[title="Hero A"]').getByText("Alpha"),
  ).toBeVisible();
  await files.getByRole("button", { name: "Back to the list" }).click();
  await expect(
    files.getByRole("button", { name: "01-hero-b.html", exact: true }),
  ).toBeVisible();
  await expect(files.locator(".canvas-page")).toHaveCount(0);
  // From the canvas: back to the canvas.
  await files.getByRole("button", { name: "Open", exact: true }).click();
  await files.getByRole("button", { name: "Open Hero B" }).click();
  await files.getByRole("button", { name: "Back to the canvas" }).click();
  await expect(files.locator(".canvas-page")).toHaveCount(3);
  // Leaving the canvas gives the list back.
  await files.getByRole("button", { name: "Back to the list" }).click();
  await expect(
    files.getByRole("button", { name: "cover.html", exact: true }),
  ).toBeVisible();
});

test("a page as tall as it reads keeps its size on the canvas once it was opened", async ({
  page: app,
  session,
  host,
}) => {
  await writeFiles(join(host.project, "design"), {
    "home.html": artboard("Home", "<p>Short</p>", "1440"),
    "hero.html": artboard("Hero", "<h1>Hero</h1>"),
  });
  const files = await openFiles(app, session, "design/");
  const tile = files
    .locator(".canvas-page", { has: app.locator('iframe[title="Home"]') })
    .locator(".page-frame");
  await expect(tile).toBeVisible();
  const height = await tile.evaluate((frame) => frame.style.height);
  // On the stage its frame is the stage's height, which is not the page's.
  await files.getByRole("button", { name: "Open Home" }).click();
  await expect(files.locator(".page-body .zoom-controls")).toBeVisible();
  await files.getByRole("button", { name: "Back to the canvas" }).click();
  await expect(files.locator(".zoom-controls output")).toBeVisible();
  await expect(tile).toBeVisible();
  expect(await tile.evaluate((frame) => frame.style.height)).toBe(height);
});

test("a page that is ready before its stage is measured opens fitted", async ({
  page: app,
  session,
  host,
}) => {
  // What the browser says of sizes comes late: the page is ready first.
  await app.addInitScript(() => {
    const Real = ResizeObserver;
    window.ResizeObserver = class extends Real {
      constructor(said) {
        super((...args) => setTimeout(() => said(...args), 1000));
      }
    };
  });
  await design(host.project);
  const files = await openFiles(app, session, "design/01-hero-a.html");
  // The first zoom it shows is the fit, never its full size.
  const level = files.locator(".page-body .zoom-controls output");
  const first = Number.parseInt(await level.textContent(), 10);
  expect(first).toBeGreaterThan(0);
  expect(first).toBeLessThan(100);
});

test("an opened page zooms as the canvas does, from the controls and over the page", async ({
  page: app,
  session,
  host,
}) => {
  await design(host.project);
  const files = await openFiles(app, session, "design");
  await files
    .getByRole("button", { name: "01-hero-a.html", exact: true })
    .click();
  await expect(
    app.frameLocator('iframe[title="Hero A"]').getByText("Alpha"),
  ).toBeVisible();
  const level = files.locator(".page-body .zoom-controls output");
  const percent = async () => Number.parseInt(await level.textContent(), 10);
  // It opens at Fit: the whole artboard, so Fit has nothing to do.
  const fitted = await percent();
  expect(fitted).toBeLessThan(100);
  await expect(files.getByRole("button", { name: "Fit" })).toBeDisabled();
  // + zooms in; the page is then larger than the pane and scrolls.
  await files.getByRole("button", { name: "Zoom in" }).click();
  await files.getByRole("button", { name: "Zoom in" }).click();
  expect(await percent()).toBeGreaterThan(fitted);
  const scroller = files.locator(".page-scroll");
  expect(
    await scroller.evaluate((box) => box.scrollWidth > box.clientWidth),
  ).toBe(true);
  // Margin-wheel zoom uses the current scale, including after prior zooms.
  // The same content point stays under the pointer on each successive step.
  for (let step = 0; step < 2; step++) {
    const anchor = await scroller.evaluate((box) => {
      const rect = box.getBoundingClientRect();
      const x = box.clientWidth / 2,
        y = box.clientHeight / 2;
      const before = { x: box.scrollLeft + x, y: box.scrollTop + y };
      box.dispatchEvent(
        new WheelEvent("wheel", {
          ctrlKey: true,
          deltaY: -10,
          cancelable: true,
          clientX: rect.left + x,
          clientY: rect.top + y,
        }),
      );
      return { before, x, y };
    });
    await expect
      .poll(async () =>
        scroller.evaluate((box, at) => {
          const ratio = Math.exp(0.1);
          return Math.abs(box.scrollLeft + at.x - at.before.x * ratio);
        }, anchor),
      )
      .toBeLessThan(2);
  }
  // ⌘ or Ctrl and the wheel over the page itself zooms too.
  const before = await percent();
  await app
    .frameLocator('iframe[title="Hero A"]')
    .locator("h1")
    .dispatchEvent("wheel", {
      ctrlKey: true,
      deltaY: -100,
      clientX: 10,
      clientY: 10,
    });
  await expect.poll(percent).toBeGreaterThan(before);
  await files.getByRole("button", { name: "Fit" }).click();
  await expect.poll(percent).toBe(fitted);
});

test("a page that runs past its size or misses a file says so", async ({
  page: app,
  session,
  host,
}) => {
  await writeFiles(join(host.project, "design/book"), {
    "book.html": artboard(
      "Book",
      '<style>section{height:300px;overflow:hidden}</style><section><div style="height:500px">Long</div></section><img src="missing.png" alt="">',
      "794",
    ),
    "02-cover.html": artboard("Cover", "<p>ok</p>"),
  });
  const files = await openFiles(app, session, "design/book/");
  await expect(files.getByText("1 page failed to run")).toBeVisible();
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    /`design\/book\/book\.html` threw: .*missing\.png did not load.*page 1 runs 200px past its height/,
  );
});

test("a slide with text over text and a document block taller than a page say so", async ({
  page: app,
  session,
  host,
}) => {
  await writeFiles(join(host.project, "design/checks"), {
    "01-slide.html": artboard(
      "Slide",
      '<p style="position:absolute;top:100px;left:100px;margin:0;font-size:40px">Revenue grew</p>' +
        '<p style="position:absolute;top:110px;left:120px;margin:0;font-size:40px">Footer note</p>' +
        '<div style="position:absolute;top:1040px;left:100px;overflow:hidden;height:20px"><p style="margin:0;font-size:40px">Cut off</p></div>' +
        "<aside>Notes stay out of the check.</aside>",
      "1920x1080 slide",
    ),
    // A5: a figure that may not break, taller than its paper inside the
    // margins; the table as tall breaks between its rows.
    "memo.html": paper(
      "Memo",
      "<style>*{margin:0}figure{break-inside:avoid}</style>" +
        '<figure style="height:1000px">tall</figure>' +
        '<table style="height:1000px"><tr><td>long</td></tr><tr><td>table</td></tr></table>',
    ),
  });
  // The one slide is a deck; the document beside it is its own.
  const files = await openFiles(app, session, "design/checks/");
  await expect(files.getByText("1 page failed to run")).toBeVisible();
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  const draft = app.locator("#prompt");
  await expect(draft).toHaveValue(
    /`design\/checks\/01-slide\.html` threw: “Cut off” runs past the slide's 1920x1080 edge.*“Revenue grew” overlaps “Footer note”/,
  );
  await files.getByRole("button", { name: "Back to the list" }).click();
  await files.getByRole("button", { name: "memo.html", exact: true }).click();
  await expect(viewerOf(files).stage.getByText("tall")).toBeVisible();
  await expect(files.getByText("1 page failed to run")).toBeVisible();
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  await expect(draft).toHaveValue(
    /`design\/checks\/memo\.html` threw: a figure is 1000px tall, more than a page \(605px\)/,
  );
});

// A deck in decks/talk, opened from the tree: the pane, its legend, the
// stage's page and the counter.
async function openDeck(app, session, project, slides, options) {
  await writeFiles(join(project, "decks/talk"), slides);
  const files = await openFiles(app, session, "decks/talk/", options);
  return {
    files,
    legend: files.getByRole("listbox", { name: "Slides" }),
    ...viewerOf(files),
  };
}

test("a deck opens in slide order, presents with its notes, and the presenter's window follows", async ({
  page: app,
  session,
  host,
}) => {
  const { files, legend, count } = await openDeck(app, session, host.project, {
    "01-cover.html": slide("Cover", "Say hello."),
    "02-plan.html": slide("Plan", "Three steps."),
    "10-end.html": slide("End"),
  });
  // No canvas: the first slide on the stage, the deck in its legend.
  await expect(files.locator(".canvas-page")).toHaveCount(0);
  await expect(count).toHaveText("1 / 3");
  await expect(legend.locator(".page-legend-name")).toHaveText([
    "1 · Cover",
    "2 · Plan",
    "3 · End",
  ]);
  // A slide's notes are under the stage, never drawn on the slide.
  await expect(files.locator(".page-main > .page-notes")).toContainText(
    "Say hello.",
  );

  await files.getByRole("button", { name: "Present" }).click();
  const stage = files.locator(".page-stage[data-presenting]");
  await expect(stage).toBeVisible();
  await expect(stage.getByText("1 / 3")).toBeVisible();
  const slideFrame = stage.frameLocator("iframe");
  await expect(
    slideFrame.getByRole("heading", { name: "Cover" }),
  ).toBeVisible();
  await expect(slideFrame.getByText("Say hello.")).toBeHidden();
  await app.keyboard.press("n");
  await expect(stage.locator(".page-notes")).toHaveText("Say hello.");
  await app.keyboard.press("ArrowRight");
  await expect(stage.getByText("2 / 3")).toBeVisible();
  await expect(stage.locator(".page-notes")).toHaveText("Three steps.");
  // Space on the focused Next presses it: one slide on, not a second for
  // the key.
  await app.keyboard.press("ArrowLeft");
  await expect(stage.getByText("1 / 3")).toBeVisible();
  await stage.getByRole("button", { name: "Next slide" }).focus();
  await app.keyboard.press("Space");
  await expect(stage.getByText("2 / 3")).toBeVisible();

  // The presenter's window: now, next and the notes; its keys move the deck.
  const [presenter] = await Promise.all([
    app.waitForEvent("popup"),
    stage.getByRole("button", { name: "Presenter view" }).click(),
  ]);
  const view = presenter.getByRole("main", { name: "Presenter view" });
  await expect(view.getByText("Slide 2 of 3")).toBeVisible();
  await expect(view.locator(".presenter-notes")).toHaveText("Three steps.");
  await presenter.keyboard.press("ArrowRight");
  await expect(view.getByText("Slide 3 of 3")).toBeVisible();
  await expect(view.getByText("End of the deck")).toBeVisible();
  await expect(stage.getByText("3 / 3")).toBeVisible();
  await stage.getByRole("button", { name: "Previous slide" }).click();
  await expect(view.getByText("Slide 2 of 3")).toBeVisible();
  await presenter.close();

  // Ended, the deck is on the slide it was presenting, in the viewer.
  await stage.getByRole("button", { name: "End" }).click();
  await expect(files.locator(".page-stage[data-presenting]")).toHaveCount(0);
  await expect(count).toHaveText("2 / 3");
  await expect(
    legend.getByRole("option", { name: "Slide 2: Plan" }),
  ).toHaveAttribute("aria-selected", "true");
  await expect(
    files.getByRole("button", { name: "Back to the list" }),
  ).toBeVisible();
});

test("a deck's legend, counter and keys step through its slides", async ({
  page: app,
  session,
  host,
}) => {
  const { files, legend, stage, count } = await openDeck(
    app,
    session,
    host.project,
    {
      "01-cover.html": slide("Cover"),
      "02-plan.html": slide("Plan"),
      "03-numbers.html": slide("Numbers"),
      "04-end.html": slide("End"),
    },
  );
  const shows = async (title, at) => {
    await expect(stage.getByRole("heading", { name: title })).toBeVisible();
    await expect(count).toHaveText(`${at} / 4`);
    await expect(
      legend.getByRole("option", { name: `Slide ${at}: ${title}` }),
    ).toHaveAttribute("aria-selected", "true");
  };
  await expect(legend.getByRole("option")).toHaveCount(4);
  await shows("Cover", 1);
  // A click in the legend goes there, and Back is still the list.
  await legend.getByRole("option", { name: "Slide 3: Numbers" }).click();
  await shows("Numbers", 3);
  // The keys: arrows and the Page keys step, round the ends; Home and End
  // go to them; Up and Down walk the legend as a list.
  for (const [key, title, at] of [
    ["ArrowRight", "End", 4],
    ["ArrowRight", "Cover", 1],
    ["End", "End", 4],
    ["Home", "Cover", 1],
    ["PageDown", "Plan", 2],
    ["ArrowDown", "Numbers", 3],
    ["ArrowUp", "Plan", 2],
    ["PageUp", "Cover", 1],
    ["ArrowLeft", "End", 4],
  ]) {
    await app.keyboard.press(key);
    await shows(title, at);
  }
  // The counter's own buttons.
  await files.getByRole("button", { name: "Previous slide" }).click();
  await shows("Numbers", 3);
  await files.getByRole("button", { name: "Next slide" }).click();
  await shows("End", 4);
  // With the slide itself in focus its keys still step, Space too, and a
  // swipe across it steps as an arrow does.
  await stage.getByRole("heading", { name: "End" }).click();
  await app.keyboard.press("Home");
  await shows("Cover", 1);
  await stage.getByRole("heading", { name: "Cover" }).click();
  await app.keyboard.press("Space");
  await shows("Plan", 2);
  await stage.locator("html").evaluate(() => {
    const touch = (type, x) =>
      dispatchEvent(
        Object.assign(new Event(type), {
          touches: [{ clientX: x, clientY: 500 }],
          changedTouches: [{ clientX: x, clientY: 500 }],
        }),
      );
    touch("touchstart", 1500);
    touch("touchend", 600);
  });
  await shows("Numbers", 3);
  // Never while a field has the keys. From the conversation the arrows
  // step, and the paging keys stay the transcript's.
  await app.locator("#prompt").focus();
  await app.keyboard.press("ArrowRight");
  await app.keyboard.press("PageDown");
  await shows("Numbers", 3);
  await app.locator("#conversation").focus();
  await app.keyboard.press("PageUp");
  await shows("Numbers", 3);
  await app.keyboard.press("ArrowRight");
  await shows("End", 4);
  // A key another control took is that control's: the pane's edge moves
  // with the arrows, and the slide stays.
  const edge = app.getByRole("separator", { name: "Resize the files" });
  const width = async () => (await files.boundingBox()).width;
  const before = await width();
  await edge.focus();
  await app.keyboard.press("ArrowRight");
  await expect.poll(width).not.toBe(before);
  await shows("End", 4);
  // The legend hides and shows; the counter stays.
  await files.getByRole("button", { name: "Legend" }).click();
  await expect(legend).toHaveCount(0);
  await files.getByRole("button", { name: "Previous slide" }).click();
  await expect(count).toHaveText("3 / 4");
  await files.getByRole("button", { name: "Legend" }).click();
  await shows("Numbers", 3);
  // Every step replaced the slide: Back is one step, to the list.
  await files.getByRole("button", { name: "Back to the list" }).click();
  await expect(
    files.getByRole("button", { name: "03-numbers.html", exact: true }),
  ).toBeVisible();
});

// The colours the screen shows at the middle of an element and at its left
// edge, read from a screenshot decoded in the page.
const shownIn = async (app, locator) => {
  const png = await locator.screenshot();
  return app.evaluate(async (data) => {
    const bytes = Uint8Array.from(atob(data), (char) => char.charCodeAt(0));
    const image = await createImageBitmap(new Blob([bytes]));
    const canvas = new OffscreenCanvas(image.width, image.height);
    const context = canvas.getContext("2d");
    context.drawImage(image, 0, 0);
    return [image.width / 2, 4].map((x) =>
      [
        ...context.getImageData(x, image.height / 2, 1, 1).data.slice(0, 3),
      ].join(),
    );
  }, png.toString("base64"));
};

test("a step shows the slide before until the next has drawn, and the stage's tools stay", async ({
  page: app,
  session,
  host,
}) => {
  await app.emulateMedia({ colorScheme: "dark" });
  const dark = (title, head) =>
    `<!doctype html><html><head><title>${title}</title><meta name="artboard" content="1920x1080 slide">${head}</head>` +
    `<body><script type="application/json" id="tweaks">{"--gap":{"editor":"range","default":8,"min":0,"max":32}}</script><h1>${title}</h1></body></html>`;
  const { files, legend, stage } = await openDeck(app, session, host.project, {
    "01-cover.html": dark("Cover", "<style>body{background:#102030}</style>"),
    "02-plan.html": dark("Plan", '<link rel="stylesheet" href="plan.css">'),
    "plan.css": "body{background:#302010}",
  });
  const box = files.locator(".page-stage");
  const shown = () => shownIn(app, box);
  await expect.poll(async () => (await shown())[0]).toBe("16,32,48");
  const cover = await shown();
  const tools = await Promise.all(
    ["Tweaks", "Zoom in"].map((name) =>
      files.getByRole("button", { name }).elementHandle(),
    ),
  );
  const kept = () =>
    Promise.all(tools.map((tool) => tool.evaluate((node) => node.isConnected)));

  // The next slide is held back twice: before it answers, and with its
  // stylesheet missing, when it has said what it is but has not drawn.
  const held = {};
  for (const [name, address] of Object.entries({
    page: /02-plan\.html/,
    style: /plan\.css/,
  }))
    await app.route(address, async (route) => {
      await new Promise((free) => {
        held[name] = free;
      });
      await route.continue();
    });
  await legend.getByRole("option", { name: "Slide 2: Plan" }).click();
  await expect.poll(() => !!held.page).toBe(true);
  expect(await shown()).toEqual(cover);
  expect(await kept()).toEqual([true, true]);
  held.page();
  await expect
    .poll(() => stage.locator("html").evaluate(() => document.readyState))
    .toBe("interactive");
  await app.evaluate(
    () =>
      new Promise((done) =>
        requestAnimationFrame(() => requestAnimationFrame(done)),
      ),
  );
  expect(await shown()).toEqual(cover);
  expect(await kept()).toEqual([true, true]);
  held.style();
  await expect.poll(async () => (await shown())[0]).toBe("48,32,16");
  expect((await shown())[1]).toBe(cover[1]);
  expect(await kept()).toEqual([true, true]);
  await expect(box.locator("iframe")).toHaveCount(1);
});

test("a long deck's legend draws only the slides near its view, and keeps the current one in it", async ({
  page: app,
  session,
  host,
}) => {
  const names = Array.from({ length: 40 }, (_, at) =>
    String(at + 1).padStart(2, "0"),
  );
  const { files, legend, count } = await openDeck(
    app,
    session,
    host.project,
    Object.fromEntries(
      names.map((name) => [`${name}.html`, slide(`S${name}`)]),
    ),
  );
  await expect(legend.getByRole("option")).toHaveCount(40);
  const first = legend.locator('iframe[title="Slide 1: S01"]');
  const last = legend.locator('iframe[title="Slide 40: S40"]');
  await expect(first).toHaveCount(1);
  await expect(last).toHaveCount(0);
  // Some of them, from the first on; the rest are their boxes only.
  const drawn = await legend.locator("iframe").count();
  expect(drawn).toBeGreaterThan(0);
  expect(drawn).toBeLessThan(40);
  await expect(legend.locator(".page-frame")).toHaveCount(40);
  // The last slide: the legend scrolls to it, and draws that end instead.
  await app.keyboard.press("End");
  await expect(count).toHaveText("40 / 40");
  await expect(
    legend.getByRole("option", { name: "Slide 40: S40" }),
  ).toBeInViewport();
  await expect(last).toHaveCount(1);
  await expect(first).toHaveCount(0);
  // A slide moved there leaves the legend where it was scrolled to.
  const scrolled = () => legend.evaluate((list) => list.scrollTop);
  const top = await scrolled();
  await files.getByRole("button", { name: "Slide actions" }).click();
  await app.getByRole("menuitem", { name: "Move earlier" }).click();
  await expect(count).toHaveText("39 / 40");
  await expect(
    legend.getByRole("option", { name: "Slide 40: S39" }),
  ).toBeVisible();
  expect(await scrolled()).toBe(top);
});

// The deck of four the tests rearrange, and its folder's files in order.
const FOUR = {
  "01-cover.html": slide("Cover"),
  "02-plan.html": slide("Plan"),
  "03-numbers.html": slide("Numbers"),
  "04-end.html": slide("End"),
};
const slidesIn = async (project) =>
  (await readdir(join(project, "decks", "talk"))).sort();

test("a deck's slide is dragged to a new place: the legend, the counter and the files follow", async ({
  page: app,
  session,
  host,
}) => {
  const { legend, stage, count } = await openDeck(
    app,
    session,
    host.project,
    FOUR,
  );
  const names = legend.locator(".page-legend-name");
  await expect(names).toHaveText([
    "1 · Cover",
    "2 · Plan",
    "3 · Numbers",
    "4 · End",
  ]);
  // From the middle of one slide to a point in another: `part` of the way
  // down it.
  const drag = async (from, to, part) => {
    const [start, end] = await Promise.all(
      [from, to].map((at) => legend.getByRole("option").nth(at).boundingBox()),
    );
    await app.mouse.move(start.x + start.width / 2, start.y + start.height / 2);
    await app.mouse.down();
    await app.mouse.move(end.x + end.width / 2, end.y + end.height * part, {
      steps: 6,
    });
  };
  // Slide 3 over the top of slide 1: a line shows where it would drop.
  await drag(2, 0, 0.2);
  await expect(legend.locator("[data-drop]")).toHaveAttribute(
    "data-path",
    "decks/talk/01-cover.html",
  );
  await app.mouse.up();
  await expect(names).toHaveText([
    "1 · Numbers",
    "2 · Cover",
    "3 · Plan",
    "4 · End",
  ]);
  // The stage stays with the slide it showed, under its new name.
  await expect(count).toHaveText("2 / 4");
  // The drag ended with the release: the pointer moving on drags nothing.
  const last = await legend.getByRole("option").nth(3).boundingBox();
  await app.mouse.move(last.x + 20, last.y + 20, { steps: 4 });
  await expect(legend.locator("[data-drop], [data-dragging]")).toHaveCount(0);
  await expect(stage.getByRole("heading", { name: "Cover" })).toBeVisible();
  expect(await slidesIn(host.project)).toEqual([
    "01-numbers.html",
    "02-cover.html",
    "03-plan.html",
    "04-end.html",
  ]);
  // The first past the last: the line is after it.
  await drag(0, 3, 0.8);
  await expect(legend.locator('[data-drop="after"]')).toHaveCount(1);
  await app.mouse.up();
  await expect(names).toHaveText([
    "1 · Cover",
    "2 · Plan",
    "3 · End",
    "4 · Numbers",
  ]);
  await expect(count).toHaveText("1 / 4");
  expect(await slidesIn(host.project)).toEqual([
    "01-cover.html",
    "02-plan.html",
    "03-end.html",
    "04-numbers.html",
  ]);
  // Dropped where it lay, nothing moves, and the press opens nothing; a
  // press that does not move opens the slide as before.
  await drag(2, 2, 0.9);
  await app.mouse.up();
  await expect(count).toHaveText("1 / 4");
  await legend.getByRole("option", { name: "Slide 3: End" }).click();
  await expect(count).toHaveText("3 / 4");
});

test("a deck's slide is moved, duplicated and deleted from its menu, by keyboard too", async ({
  page: app,
  session,
  host,
}) => {
  const { files, legend, stage, count } = await openDeck(
    app,
    session,
    host.project,
    FOUR,
  );
  const names = legend.locator(".page-legend-name");
  const actions = files.getByRole("button", { name: "Slide actions" });
  const item = (name) => app.getByRole("menuitem", { name });
  await expect(names).toHaveCount(4);
  // By keyboard: the first slide cannot move earlier, so the menu opens on
  // Move later; the button keeps the focus for the next move.
  await actions.focus();
  await app.keyboard.press("Enter");
  await expect(item("Move earlier")).toBeDisabled();
  await expect(item("Move later")).toBeFocused();
  await app.keyboard.press("Enter");
  await expect(names).toHaveText([
    "1 · Plan",
    "2 · Cover",
    "3 · Numbers",
    "4 · End",
  ]);
  await expect(count).toHaveText("2 / 4");
  await expect(actions).toBeFocused();
  await app.keyboard.press("Enter");
  await expect(item("Move earlier")).toBeFocused();
  await app.keyboard.press("Enter");
  await expect(count).toHaveText("1 / 4");
  expect(await slidesIn(host.project)).toEqual(Object.keys(FOUR));
  // A right click on a slide opens the menu for that slide.
  await legend
    .getByRole("option", { name: "Slide 2: Plan" })
    .click({ button: "right" });
  await expect(app.getByRole("menu", { name: "Slide actions" })).toBeVisible();
  await expect(count).toHaveText("2 / 4");
  // Duplicate: a copy right after it, named for it; the stage stays.
  await item("Duplicate").click();
  await expect(names).toHaveText([
    "1 · Cover",
    "2 · Plan",
    "3 · Plan",
    "4 · Numbers",
    "5 · End",
  ]);
  await expect(count).toHaveText("2 / 5");
  expect(await slidesIn(host.project)).toEqual([
    "01-cover.html",
    "02-plan.html",
    "03-plan-copy.html",
    "04-numbers.html",
    "05-end.html",
  ]);
  // Delete asks first, and says it cannot be undone.
  await actions.click();
  await item("Delete…").click();
  const ask = app.getByRole("dialog", { name: "Delete slide" });
  await expect(ask).toContainText(
    "Plan (02-plan.html) is deleted from the folder. This cannot be undone.",
  );
  await ask.getByRole("button", { name: "Cancel" }).click();
  await expect(names).toHaveCount(5);
  // The open slide deleted, the stage shows the next; no other is renamed.
  await actions.click();
  await item("Delete…").click();
  const confirm = ask.getByRole("button", {
    name: "Delete slide",
    exact: true,
  });
  await confirm.click();
  await expect(names).toHaveText([
    "1 · Cover",
    "2 · Plan",
    "3 · Numbers",
    "4 · End",
  ]);
  await expect(count).toHaveText("2 / 4");
  expect(await slidesIn(host.project)).toEqual([
    "01-cover.html",
    "03-plan-copy.html",
    "04-numbers.html",
    "05-end.html",
  ]);
  // The last one deleted while open, the one before it is shown.
  await legend.getByRole("option", { name: "Slide 4: End" }).click();
  await actions.click();
  await item("Delete…").click();
  await confirm.click();
  await expect(count).toHaveText("3 / 3");
  await expect(stage.getByRole("heading", { name: "Numbers" })).toBeVisible();
});

test("a move the folder no longer allows is refused: nothing moves, and the pane says so", async ({
  page: app,
  session,
  host,
}) => {
  const { files, legend, count } = await openDeck(
    app,
    session,
    host.project,
    FOUR,
  );
  const names = legend.locator(".page-legend-name");
  await expect(names).toHaveCount(4);
  // Behind the pane's back, as the agent would.
  const dir = join(host.project, "decks", "talk");
  await rename(join(dir, "02-plan.html"), join(dir, "02-agenda.html"));
  const down = async () => {
    await files.getByRole("button", { name: "Slide actions" }).click();
    await app.getByRole("menuitem", { name: "Move later" }).click();
  };
  await down();
  const notice = files.locator(".page-notice");
  await expect(notice).toHaveText(
    "The deck's folder changed meanwhile, so nothing was moved.",
  );
  expect(await slidesIn(host.project)).toEqual([
    "01-cover.html",
    "02-agenda.html",
    "03-numbers.html",
    "04-end.html",
  ]);
  // The folder was read again: the same move now goes through.
  await expect(
    legend.locator('[data-path="decks/talk/02-agenda.html"]'),
  ).toHaveCount(1);
  await expect(count).toHaveText("1 / 4");
  await down();
  await expect(count).toHaveText("2 / 4");
  await expect(notice).toHaveCount(0);
  expect(await slidesIn(host.project)).toEqual([
    "01-agenda.html",
    "02-cover.html",
    "03-numbers.html",
    "04-end.html",
  ]);
});

test("on a phone a deck's legend is a filmstrip under the stage", async ({
  page: app,
  session,
  host,
}) => {
  const { files, legend, stage, count } = await openDeck(
    app,
    session,
    host.project,
    Object.fromEntries(
      [
        "Cover",
        "Plan",
        "Numbers with a name too long for its picture",
        "Risks",
        "End",
      ].map((title, at) => [`0${at + 1}.html`, slide(title)]),
    ),
    { phone: true },
  );
  await expect(stage.getByRole("heading", { name: "Cover" })).toBeVisible();
  await expect(legend.getByRole("option")).toHaveCount(5);
  // One row under the stage, scrolling sideways.
  const strip = await legend.evaluate((list) => ({
    under:
      list.getBoundingClientRect().top >=
      list
        .closest(".page-body")
        .querySelector(".page-stage")
        .getBoundingClientRect().bottom,
    tops: new Set(
      [...list.children].map((item) => item.getBoundingClientRect().top),
    ).size,
    scrolls: list.scrollWidth > list.clientWidth,
    // A slide's name is as wide as its picture, however long.
    widths: new Set([...list.children].map((item) => item.offsetWidth)).size,
  }));
  expect(strip).toEqual({ under: true, tops: 1, scrolls: true, widths: 1 });
  // A tap goes there, and the strip brings the current one into view.
  await legend.getByRole("option", { name: "Slide 5: End" }).click();
  await expect(stage.getByRole("heading", { name: "End" })).toBeVisible();
  await expect(count).toHaveText("5 / 5");
  await files.getByRole("button", { name: "Previous slide" }).click();
  await files.getByRole("button", { name: "Previous slide" }).click();
  await files.getByRole("button", { name: "Previous slide" }).click();
  await files.getByRole("button", { name: "Previous slide" }).click();
  await expect(
    legend.getByRole("option", { name: "Slide 1: Cover" }),
  ).toBeInViewport();
});

test("a slide that fails is marked in the legend, and its errors go to the agent once", async ({
  page: app,
  session,
  host,
}) => {
  const { files, legend, stage } = await openDeck(app, session, host.project, {
    "01-cover.html": slide("Cover"),
    "02-broken.html": slide(
      "Broken",
      "",
      '<script>dispatchEvent(new ErrorEvent("error", { message: "ReferenceError: missing is not defined", lineno: 3 }))</script>',
    ),
  });
  const broken = legend.getByRole("option", {
    name: "Slide 2: Broken, failed to run",
  });
  await expect(broken).toBeVisible();
  await expect(
    legend.getByRole("option", { name: "Slide 1: Cover", exact: true }),
  ).toBeVisible();
  await expect(files.getByText("1 page failed to run")).toBeVisible();
  // On the stage too it says the same error, which is kept once.
  await broken.click();
  await expect(stage.getByRole("heading", { name: "Broken" })).toBeVisible();
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    "`decks/talk/02-broken.html` threw: ReferenceError: missing is not defined (line 3)",
  );
});

test("find in a deck steps across its slides and marks what it found", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, count } = await openDeck(app, session, host.project, {
    "01-cover.html": slide(
      "Cover",
      "Revenue is in the notes.",
      "<p>Revenue up</p>",
    ),
    "02-plan.html": slide("Plan", "", "<p>Nothing here</p>"),
    "03-numbers.html": slide(
      "Numbers",
      "",
      "<p>Revenue per quarter</p><p>All REVENUE, no costs</p><script>/* revenue */</script>",
    ),
  });
  const marks = (name) =>
    stage
      .locator("html")
      .evaluate((_, name) => CSS.highlights.get(name)?.size ?? 0, name);
  await files.getByRole("button", { name: "Find" }).click();
  const bar = files.getByRole("search", { name: "Find in this deck" });
  const field = bar.getByRole("searchbox");
  await expect(field).toBeFocused();
  // What a slide draws is counted: not its notes, nor its scripts.
  await field.fill("revenue");
  await expect(bar).toContainText("1 of 3");
  await expect.poll(() => marks("uagent-find")).toBe(1);
  await expect.poll(() => marks("uagent-found")).toBe(1);
  // The next one is two slides on; the slide between has none.
  await field.press("Enter");
  await expect(bar).toContainText("2 of 3");
  await expect(count).toHaveText("3 / 3");
  await expect(stage.getByRole("heading", { name: "Numbers" })).toBeVisible();
  await expect.poll(() => marks("uagent-find")).toBe(2);
  await field.press("Enter");
  await expect(bar).toContainText("3 of 3");
  await expect(count).toHaveText("3 / 3");
  // Round the end to the first, and back with Shift.
  await field.press("Enter");
  await expect(bar).toContainText("1 of 3");
  await expect(count).toHaveText("1 / 3");
  await field.press("Shift+Enter");
  await expect(bar).toContainText("3 of 3");
  await expect(count).toHaveText("3 / 3");
  await expect(stage.getByRole("heading", { name: "Numbers" })).toBeVisible();
  await field.fill("wombat");
  await expect(bar).toContainText("No matches");
  await expect.poll(() => marks("uagent-find")).toBe(0);
  // Closed, nothing stays marked.
  await field.fill("revenue");
  await expect.poll(() => marks("uagent-find")).toBe(2);
  await field.press("Escape");
  await expect(bar).toHaveCount(0);
  await expect.poll(() => marks("uagent-find")).toBe(0);
});

test("what a slide says as it is left is never taken for the next slide's", async ({
  page: app,
  session,
  host,
}) => {
  const { files, legend, stage } = await openDeck(app, session, host.project, {
    "01-cover.html": slide(
      "Cover",
      "",
      '<script>addEventListener("beforeunload", () => dispatchEvent(new ErrorEvent("error", { message: "late from the cover" })))</script>',
    ),
    "02-plan.html": slide("Plan"),
  });
  await expect(stage.getByRole("heading", { name: "Cover" })).toBeVisible();
  await files.getByRole("button", { name: "Next slide" }).click();
  await expect(stage.getByRole("heading", { name: "Plan" })).toBeVisible();
  await files.getByRole("button", { name: "Next slide" }).click();
  await expect(stage.getByRole("heading", { name: "Cover" })).toBeVisible();
  await expect(
    legend.getByRole("option", { name: "Slide 2: Plan", exact: true }),
  ).toBeVisible();
  await expect(files.getByText(/failed to run/)).toHaveCount(0);
});

test("a design's page opens among its pages; a document beside them is its own, and skipped", async ({
  page: app,
  session,
  host,
}) => {
  const titled = (title) => artboard(title, `<h1>${title}</h1>`);
  await writeFiles(join(host.project, "design"), {
    "01-home.html": titled("Home"),
    "02-pricing.html": titled("Pricing"),
    "03-about.html": titled("About"),
    "terms.html": paper("Terms", "<h1>Terms</h1>", "a4"),
  });
  const files = await openFiles(app, session, "design/");
  // The canvas shows all four.
  await expect(files.locator(".canvas-page")).toHaveCount(4);
  await files.getByRole("button", { name: "Open Pricing" }).click();
  const legend = files.getByRole("listbox", { name: "Pages" });
  const { stage, count } = viewerOf(files);
  await expect(legend.locator(".page-legend-name")).toHaveText([
    "1 · Home",
    "2 · Pricing",
    "3 · About",
  ]);
  const shows = async (title, at) => {
    await expect(stage.getByRole("heading", { name: title })).toBeVisible();
    await expect(count).toHaveText(`${at} / 3`);
    await expect(
      legend.getByRole("option", { name: `Page ${at}: ${title}` }),
    ).toHaveAttribute("aria-selected", "true");
  };
  await shows("Pricing", 2);
  // Round the three by key, counter and legend, never onto the document.
  await app.keyboard.press("ArrowRight");
  await shows("About", 3);
  await files.getByRole("button", { name: "Next page" }).click();
  await shows("Home", 1);
  await files.getByRole("button", { name: "Previous page" }).click();
  await shows("About", 3);
  await legend.getByRole("option", { name: "Page 2: Pricing" }).click();
  await shows("Pricing", 2);
  await files.getByRole("button", { name: "Back to the canvas" }).click();
  // The document opens as itself, with no pages of the design beside it.
  await files
    .getByRole("button", { name: "Open document Terms, 1 page" })
    .click();
  await expect(stage.getByRole("heading", { name: "Terms" })).toBeVisible();
  await expect(files.getByRole("listbox")).toHaveCount(0);
});

// A picture and, with `film`, a short film drawn in the page, as the bytes
// of their files: each browser records the kind of film it plays.
const drawnMedia = (app, film) =>
  app.evaluate(async (film) => {
    const canvas = Object.assign(document.createElement("canvas"), {
      width: 320,
      height: 180,
    });
    const pen = canvas.getContext("2d");
    pen.fillRect(0, 0, 320, 180);
    const png = canvas.toDataURL("image/png").split(",")[1];
    if (!film) return { png };
    const recorder = new MediaRecorder(canvas.captureStream());
    const parts = [];
    recorder.ondataavailable = (event) => parts.push(event.data);
    const stopped = new Promise((done) => {
      recorder.onstop = done;
    });
    recorder.start();
    for (let frame = 0; frame < 12; frame++) {
      pen.fillStyle = frame % 2 ? "white" : "black";
      pen.fillRect(0, 0, 320, 180);
      await new Promise(requestAnimationFrame);
    }
    recorder.stop();
    await stopped;
    const bytes = new Uint8Array(
      await new Blob(parts, { type: recorder.mimeType }).arrayBuffer(),
    );
    return {
      png,
      film: btoa(
        Array.from(bytes, (byte) => String.fromCharCode(byte)).join(""),
      ),
      type: recorder.mimeType,
    };
  }, film);
// A 32 by 16 AVIF, which no canvas writes.
const AVIF =
  "AAAAIGZ0eXBhdmlmAAAAAE1pUHJhdmlmbWlhZm1pZjEAAAEhbWV0YQAAAAAAAAAhaGRscgAAAAAAAAAAcGljdAAAAAAAAAAAAAAAAAAAAAAkZGluZgAAABxkcmVmAAAAAAAAAAEAAAAMdXJsIAAAAAEAAAAOcGl0bQAAAAAAAQAAACNpaW5mAAAAAAABAAAAFWluZmUCAAAAAAEAAGF2MDEAAAAAgWlwcnAAAABgaXBjbwAAABNjb2xybmNseAACAAIABoAAAAAMY2xsaQDLAEAAAAAUaXNwZQAAAAAAAAAgAAAAEAAAAAlpcm90AAAAABBwaXhpAAAAAAMICAgAAAAMYXYxQ4EADAAAAAAZaXBtYQAAAAAAAAABAAEGgQIDBYaEAAAAHmlsb2MAAAAARAAAAQABAAAAAQAAAVEAAAAoAAAAAW1kYXQAAAAAAAAAOBIACg0AAAACH/z/+BAgIGhAMhUQAZIACCCCILUTJz7o2Nek8W4tctY=";

test("a design's canvas shows its pictures and films, and a document or a deck in it as a stack", async ({
  page: app,
  session,
  host,
  browserName,
}) => {
  await openSession(app, session);
  // A film and an AVIF are checked in Chromium, which records the one and
  // reads the other on every system; WebKit on Linux does neither.
  const rich = browserName === "chromium";
  const media = await drawnMedia(app, rich);
  const clip = media.type?.includes("webm") ? "clip.webm" : "clip.mp4";
  await writeFiles(join(host.project, "design"), {
    "01-home.html": artboard("Home", "<h1>Home</h1>"),
    "02-pricing.html": artboard("Pricing", "<h1>Pricing</h1>"),
    "hero.png": Buffer.from(media.png, "base64"),
    ...(rich && {
      "photo.avif": Buffer.from(AVIF, "base64"),
      [clip]: Buffer.from(media.film, "base64"),
    }),
    "mark.svg":
      '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100"><rect width="200" height="100"/></svg>',
    "broken.png": "not a picture",
    "memo.html": paper(
      "Memo",
      '<h1>Memo</h1><h2 style="break-before:page">Two</h2>',
    ),
    "talk/01-cover.html": slide("Cover"),
    "talk/02-plan.html": slide("Plan"),
    "talk/03-end.html": slide("End"),
  });
  const files = await openFiles(app, session, "design/");
  // Everything the folder holds lies on the canvas, each named for what a
  // click opens.
  for (const name of [
    "Open Home",
    "Open Pricing",
    "Open hero.png",
    "Open mark.svg",
    ...(rich ? ["Open photo.avif", `Open ${clip}`] : []),
    "Open broken.png",
    "Open document Memo, 2 pages",
    "Open deck talk, 3 slides",
  ])
    await expect(
      files.getByRole("button", { name, exact: true }),
    ).toBeVisible();
  await expect(files.locator(".canvas-page")).toHaveCount(rich ? 9 : 7);
  // A picture and a film are drawn as themselves, at their own size.
  const sizeOf = (media) =>
    files
      .locator(".canvas-page .page-frame", { has: app.locator(media) })
      .evaluate((box) => [box.style.width, box.style.height]);
  for (const [media, size] of [
    ['img[src*="hero.png"]', ["320px", "180px"]],
    ['img[src*="mark.svg"]', ["200px", "100px"]],
    ...(rich
      ? [
          ['img[src*="photo.avif"]', ["32px", "16px"]],
          [`video[controls][src*="${clip}"]`, ["320px", "180px"]],
        ]
      : []),
  ])
    await expect.poll(() => sizeOf(media)).toEqual(size);
  // What is no picture is marked and says so by its name, as its box and
  // not a broken image; the stacks say how many they hold.
  await expect(files.locator(".canvas-page[data-failed]")).toHaveCount(1);
  await expect(files.locator(".canvas-page[data-failed] img")).toHaveCount(0);
  await expect(
    files.locator(".canvas-name", { hasText: "broken.png" }),
  ).toHaveText("broken.png did not load");
  await expect(files.locator(".canvas-count")).toHaveText([
    "2 pages",
    "3 slides",
  ]);
  await expect(files.getByText(/failed to run/)).toHaveCount(0);
  const back = files.getByRole("button", { name: "Back to the canvas" });
  // A picture opens as its file, to zoom; Back is the canvas.
  await files.getByRole("button", { name: "Open hero.png" }).click();
  await expect(files.locator(".files-image img")).toBeVisible();
  await back.click();
  if (rich) {
    await files.getByRole("button", { name: "Open photo.avif" }).click();
    await expect
      .poll(() =>
        files
          .locator(".files-image img")
          .evaluate((image) => image.naturalWidth),
      )
      .toBe(32);
    await back.click();
    // A film opens as its file, to play.
    await files.getByRole("button", { name: `Open ${clip}` }).click();
    await expect
      .poll(() =>
        files
          .locator(".files-image video[controls]")
          .evaluate((film) => film.videoWidth),
      )
      .toBe(320);
    await back.click();
  }
  // A deck's stack opens the deck at its first slide, with its own legend.
  const { stage, count } = viewerOf(files);
  await files.getByRole("button", { name: "Open deck talk, 3 slides" }).click();
  await expect(stage.getByRole("heading", { name: "Cover" })).toBeVisible();
  await expect(
    files.getByRole("listbox", { name: "Slides" }).getByRole("option"),
  ).toHaveCount(3);
  await files.getByRole("button", { name: "Next slide" }).click();
  await expect(stage.getByRole("heading", { name: "Plan" })).toBeVisible();
  await back.click();
  // A document's stack opens the document.
  await files
    .getByRole("button", { name: "Open document Memo, 2 pages" })
    .click();
  await expect(stage.getByRole("heading", { name: "Memo" })).toBeVisible();
  await expect(count).toHaveText("1 / 2");
  await back.click();
  // A page's legend still lists the pages only.
  await files.getByRole("button", { name: "Open Home" }).click();
  await expect(
    files.getByRole("listbox", { name: "Pages" }).locator(".page-legend-name"),
  ).toHaveText(["1 · Home", "2 · Pricing"]);
});

test("find follows what the agent rewrites while it is open", async ({
  page: app,
  session,
  host,
  command,
}) => {
  await design(host.project);
  const files = await openFiles(app, session, "design/");
  await files.getByRole("button", { name: "Open Hero B" }).click();
  await files.getByRole("button", { name: "Find" }).click();
  const bar = files.getByRole("search", { name: "Find in this design" });
  await bar.getByRole("searchbox").fill("rewritten");
  await expect(bar).toContainText("No matches");
  await selectProbeModel(command, session, { yolo: true });
  await app.locator("#prompt").fill("Design probe");
  await app.locator("#prompt").press("Enter");
  // The page it rewrote is read again, and holds the words now.
  await expect(bar).toContainText("1 of 1");
  await expect(
    viewerOf(files).stage.getByRole("heading", { name: "Rewritten" }),
  ).toBeVisible();
});

// docs/memo.html, opened from the tree: an A5 document of ten parts, each
// starting a page and shorter than one, so it has ten pages.
const MEMO_PAGES = 10;
async function openMemo(app, session, project, options) {
  const memo = join(project, "docs/memo.html");
  const parts = Array.from({ length: MEMO_PAGES }, (_, part) =>
    Array.from(
      { length: 12 },
      (_, at) => `<p>Paragraph ${part * 12 + at + 1} of the memo.</p>`,
    ).join(""),
  ).join('<h2 style="break-before:page">Next part</h2>');
  await writeFiles(project, {
    "docs/memo.html": paper(
      "Memo",
      "<style>*{margin:0}p{height:30px}</style>" +
        "<h1>Q3 memo</h1><p>Revenue grew.</p><ul><li>One</li><li>Two</li></ul>" +
        "<table><tr><th>Ask</th><th>When</th></tr><tr><td>Hire</td><td>Nov</td></tr></table>" +
        parts,
    ),
  });
  const files = await openFiles(app, session, "docs/memo.html", options);
  const { stage: page, count } = viewerOf(files);
  return { files, memo, page, count };
}
// A length of the page in millimetres, to a tenth.
const mm = (px) => Math.round((px * 254) / 96) / 10;
// Whether its first line is on the sheet and not cut away with another page.
const onSheet = (text) =>
  text.evaluate((node) => {
    const range = document.createRange();
    range.selectNodeContents(node);
    const { x, y, width, height } = range.getClientRects()[0];
    return node.contains(
      document.elementFromPoint(x + width / 2, y + height / 2),
    );
  });

test("a document is stepped a sheet of its paper at a time, by its legend, its counter and the keys, in a frame that never loads again", async ({
  page: app,
  session,
  host,
}) => {
  const { files, page, count } = await openMemo(app, session, host.project);
  const legend = files.getByRole("listbox", { name: "Pages" });
  const pages = MEMO_PAGES;
  const shows = async (at) => {
    await expect(count).toHaveText(`${at} / ${pages}`);
    await expect(
      legend.getByRole("option", { name: `Page ${at}: Memo` }),
    ).toHaveAttribute("aria-selected", "true");
    await expect(
      page.getByText(`Paragraph ${(at - 1) * 12 + 1} of the memo.`),
    ).toBeInViewport();
  };
  await shows(1);
  await expect(legend.getByRole("option")).toHaveCount(pages);
  // One sheet of A5, whatever the pane's size.
  expect(
    await page.locator("html").evaluate(() => [innerWidth, innerHeight]),
  ).toEqual([559, 794]);
  await page.locator("html").evaluate(() => {
    window.loaded = "once";
  });
  await expect(
    files.getByRole("button", { name: "Previous page" }),
  ).toBeDisabled();
  await legend.getByRole("option", { name: "Page 2: Memo" }).click();
  await shows(2);
  await expect(page.getByText("Paragraph 1 of the memo.")).not.toBeInViewport();
  // The keys move by page, to the ends too; the legend is walked as a list.
  for (const [key, at] of [
    ["PageDown", 3],
    ["PageUp", 2],
    ["ArrowLeft", 1],
    ["End", pages],
    ["Home", 1],
    ["ArrowDown", 2],
  ]) {
    await app.keyboard.press(key);
    await shows(at);
  }
  await files.getByRole("button", { name: "Next page" }).click();
  await shows(3);
  expect(await page.locator("html").evaluate(() => window.loaded)).toBe("once");
});

test("a document opened alone turns its pages by the keys", async ({
  page: app,
  session,
  host,
}) => {
  const { files } = await openMemo(app, session, host.project);
  const alone = await app.context().newPage();
  await alone.goto(
    await files.getByRole("link", { name: "Open alone" }).getAttribute("href"),
  );
  const shows = async (page) => {
    await expect
      .poll(() =>
        onSheet(
          alone.getByText(`Paragraph ${(page - 1) * 12 + 1} of the memo.`),
        ),
      )
      .toBe(true);
    expect(await alone.evaluate(() => location.hash)).toBe(`#${page}`);
  };
  for (const [key, page] of [
    ["PageDown", 2],
    ["ArrowRight", 3],
    ["End", MEMO_PAGES],
    ["PageDown", MEMO_PAGES],
    ["ArrowLeft", MEMO_PAGES - 1],
    ["Home", 1],
    ["PageUp", 1],
  ]) {
    await alone.keyboard.press(key);
    await shows(page);
  }
});

test("on a phone a document's pages are a filmstrip under its sheet", async ({
  page: app,
  session,
  host,
}) => {
  const { files, page, count } = await openMemo(app, session, host.project, {
    phone: true,
  });
  const legend = files.getByRole("listbox", { name: "Pages" });
  await expect(count).toHaveText(`1 / ${MEMO_PAGES}`);
  const sheet = await files.locator(".page-stage").boundingBox();
  expect((await legend.boundingBox()).y).toBeGreaterThanOrEqual(
    sheet.y + sheet.height,
  );
  await legend.getByRole("option", { name: "Page 2: Memo" }).click();
  await expect(count).toHaveText(`2 / ${MEMO_PAGES}`);
  await expect(page.getByText("Paragraph 13 of the memo.")).toBeInViewport();
});

test("a document's margins are its print line's, and its title page fills the first sheet inside them", async ({
  page: app,
  session,
  host,
}) => {
  // The margins from the top clockwise, in millimetres.
  const formats = {
    "": [25, 25, 25, 25],
    narrow: [12.7, 12.7, 12.7, 12.7],
    moderate: [25, 19, 25, 19],
    wide: [25, 50, 25, 50],
    "20mm 18mm": [20, 18, 20, 18],
    "2cm 1in": [20, 25.4, 20, 25.4],
    "landscape 10mm 20mm 30mm 40mm": [10, 20, 30, 40],
    "narrow landscape": [12.7, 12.7, 12.7, 12.7],
  };
  const name = (said) => `${said.replaceAll(" ", "-") || "normal"}.html`;
  await writeFiles(
    join(host.project, "docs"),
    Object.fromEntries(
      Object.keys(formats).map((said) => [
        name(said),
        `<!doctype html><html><head><title>Margins</title><meta name="print" content="flow a4 ${said}">` +
          "<style>@page{margin:5mm}body{padding:40px}section.title-page{height:300px;min-height:120vh}</style></head>" +
          '<body><section class="title-page"><h1>Title</h1></section><p>After the title.</p></body></html>',
      ]),
    ),
  );
  const files = await openFiles(app, session, "docs");
  const { stage: page, count } = viewerOf(files);
  for (const [said, [top, right, bottom, left]] of Object.entries(formats)) {
    await files.getByRole("button", { name: name(said), exact: true }).click();
    await expect(count).toHaveText("1 / 2");
    const [w, h] = said.includes("landscape") ? [297, 210] : [210, 297];
    const boxes = await page.locator(".title-page").evaluate((title) => {
      const { x, y, width, height } = title.getBoundingClientRect();
      const sheet = document.documentElement.getBoundingClientRect();
      return [sheet.width, sheet.height, x, y, width, height];
    });
    // The sheet, then the title page: where it starts and what it covers.
    expect(boxes.map(mm)).toEqual(
      [w, h, left, top, w - left - right, h - top - bottom].map(
        (length) => Math.round(length * 10) / 10,
      ),
    );
    expect(await onSheet(page.getByText("After the title."))).toBe(false);
    await app.keyboard.press("PageDown");
    await expect(count).toHaveText("2 / 2");
    await expect
      .poll(() => onSheet(page.getByText("After the title.")))
      .toBe(true);
    await files.getByRole("button", { name: "Back to the list" }).click();
  }
});

test("a document's pages are the browser's: any block has its page, and a link or the focus turns to it", async ({
  page: app,
  session,
  host,
  browserName,
}) => {
  // A5 inside its margins is 605px of text.
  await writeFiles(join(host.project, "docs"), {
    "letter.html": paper(
      "Letter",
      "<style>*{margin:0}table{break-inside:avoid}tr{height:100px}</style>" +
        '<div style="height:120px">Letterhead</div>' +
        '<p><a href="#end">To the end</a></p>' +
        `<div>${"Bare text. ".repeat(600)}</div>` +
        '<h2 id="kept" style="break-after:avoid">Kept with its table</h2>' +
        "<table><tr><td>First row.</td></tr><tr><td>Second row.</td></tr><tr><td>Third row.</td></tr></table>" +
        '<section style="break-before:page"><h2 id="end">The end</h2>' +
        '<p><a href="https://example.com/">Elsewhere</a></p></section>' +
        "<aside>Trailing aside.</aside>",
    ),
  });
  const files = await openFiles(app, session, "docs/letter.html");
  const { stage: page, count } = viewerOf(files);
  await expect(count).toHaveText(/^1 \/ \d+$/);
  const pages = Number((await count.textContent()).split(" / ")[1]);
  expect(pages).toBeGreaterThan(3);
  expect(await onSheet(page.getByText("Letterhead"))).toBe(true);
  expect(await onSheet(page.getByText("Trailing aside."))).toBe(false);

  // A link to a place in it turns to that place's page, and the pane follows.
  await page.getByRole("link", { name: "To the end" }).click();
  await expect(count).toHaveText(`${pages} / ${pages}`);
  const end = page.getByRole("heading", { name: "The end" });
  for (const text of [end, page.getByText("Trailing aside.")])
    await expect.poll(() => onSheet(text)).toBe(true);
  expect(await page.locator("html").evaluate(() => [scrollX, scrollY])).toEqual(
    [0, 0],
  );
  // The page before: the table moved there whole, and where the browser
  // keeps a heading with what follows it (WebKit does not), its heading too.
  await app.keyboard.press("ArrowLeft");
  await expect(count).toHaveText(`${pages - 1} / ${pages}`);
  for (const text of ["First row.", "Third row."])
    await expect.poll(() => onSheet(page.getByText(text))).toBe(true);
  if (browserName !== "webkit")
    expect(await onSheet(page.getByText("Kept with its table"))).toBe(true);

  // The focus led to another page turns to it too.
  await page.getByRole("link", { name: "Elsewhere" }).focus();
  await expect(count).toHaveText(`${pages} / ${pages}`);
  await expect.poll(() => onSheet(end)).toBe(true);
});

test("a document's forced breaks, dead links and wide lines: the pages are those it has, and a link turns to its own", async ({
  page: app,
  session,
  host,
}) => {
  // A5 with no margins: every page starts at the sheet's left edge.
  const memo = join(host.project, "docs/breaks.html");
  await writeFiles(join(host.project, "docs"), {
    "breaks.html": paper(
      "Breaks",
      "<style>*{margin:0}p{height:100px}</style>" +
        "<p>Opening.</p>" +
        '<p style="page-break-before:always;break-after:page">Alone.</p>' +
        '<p>Third. <a href="#nowhere">Nowhere</a> <a href="#">Top</a> <a href="#3">Digits</a></p>' +
        `<pre>${"x".repeat(400)}</pre>` +
        '<h2 id="3" style="break-before:left">Named three</h2>' +
        '<blockquote><p>Quote.</p><p style="break-before:page">Second part.</p></blockquote>',
      "a5 0mm",
    ),
  });
  const files = await openFiles(app, session, "docs/breaks.html");
  const { stage: page, count } = viewerOf(files);
  const on = async (at, shown, cut) => {
    await expect(count).toHaveText(`${at} / 5`);
    await expect.poll(() => onSheet(page.getByText(shown))).toBe(true);
    expect(await onSheet(page.getByText(cut))).toBe(false);
  };
  await on(1, "Opening.", "Alone.");
  // The line wider than the text is said: print would shrink every page.
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    /a pre is \d+px wide, more than the page's text \(559px\)/,
  );
  await page.locator("html").click();
  await app.keyboard.press("PageDown");
  await on(2, "Alone.", "Third.");
  await app.keyboard.press("PageDown");
  await on(3, "Third.", "Named three");
  // A link to nowhere leaves the page; one to an id of digits is no page's
  // number; "#" is the first page.
  await page.getByRole("link", { name: "Nowhere" }).click();
  await on(3, "Third.", "Named three");
  await page.getByRole("link", { name: "Digits" }).click();
  await on(4, "Named three", "Second part.");
  // The wide line of the page before is cut at its text's edge, not drawn
  // across this page.
  expect(await onSheet(page.locator("pre"))).toBe(false);
  await app.keyboard.press("PageUp");
  await on(3, "Third.", "Named three");
  await page.getByRole("link", { name: "Top" }).click();
  await on(1, "Opening.", "Alone.");

  // A block typed in is saved as written, without what marks its breaks here.
  await app.keyboard.press("End");
  await on(5, "Second part.", "Quote.");
  await app.keyboard.press("PageUp");
  await files.getByRole("button", { name: "Edit" }).click();
  await page.getByText("Quote.", { exact: true }).click();
  await app.keyboard.press("End");
  await app.keyboard.type(" More.");
  await page.getByText("Named three").click();
  await expect(files.locator(".page-tools").getByRole("status")).toHaveText(
    "Saved",
  );
  const { readFile } = await import("node:fs/promises");
  expect(await readFile(memo, "utf8")).toContain(
    '<blockquote><p>Quote. More.</p><p style="break-before:page">Second part.</p></blockquote>',
  );
});

test("a print line that is not understood, or whose margins leave no room, says so", async ({
  page: app,
  session,
  host,
}) => {
  const TEXT = "<style>*{margin:0}</style><p>Text.</p>";
  await writeFiles(join(host.project, "docs"), {
    "odd.html": paper("Odd", TEXT, "a4 wide constructor 3em"),
    "card.html": paper("Card", TEXT, "90x50mm"),
    "flat.html": paper("Flat", TEXT, "0x297mm"),
  });
  const files = await openFiles(app, session, "docs");
  const { stage: page, count } = viewerOf(files);
  const draft = app.locator("#prompt");
  for (const [name, said, sheet] of [
    [
      "odd.html",
      /"constructor" in the print line is not understood; "3em" in the print line/,
      [210, 297, 50, 25],
    ],
    [
      "card.html",
      /the margins leave no room on the 90x50mm paper/,
      [90, 50, 0, 0],
    ],
    [
      "flat.html",
      /"0x297mm" in the print line is not understood/,
      [210, 297, 25, 25],
    ],
  ]) {
    await files.getByRole("button", { name, exact: true }).click();
    await expect(count).toHaveCount(0);
    // The sheet, and where its text starts: the margins understood, or none.
    expect(
      (
        await page.getByText("Text.").evaluate((text) => {
          const { x, y } = text.getBoundingClientRect();
          const sheet = document.documentElement.getBoundingClientRect();
          return [sheet.width, sheet.height, x, y];
        })
      ).map(mm),
    ).toEqual(sheet);
    await files.getByRole("button", { name: "Ask agent to fix" }).click();
    await expect(draft).toHaveValue(said);
    await draft.fill("");
    await files.getByRole("button", { name: "Back to the list" }).click();
  }
});

test("a long table breaks between its rows across a document's sheets, and prints on as many", async ({
  page: app,
  session,
  host,
  browserName,
}) => {
  // A5 inside its margins holds 20 rows of 30px.
  await writeFiles(join(host.project, "docs"), {
    "rows.html": paper(
      "Rows",
      "<style>*{margin:0;padding:0}table{border-collapse:collapse}tr{height:30px}</style>" +
        `<table>${Array.from({ length: 60 }, (_, at) => `<tr><td>Row ${at + 1}.</td></tr>`).join("")}</table>`,
    ),
  });
  const files = await openFiles(app, session, "docs/rows.html");
  const { stage: page, count } = viewerOf(files);
  await expect(count).toHaveText("1 / 3");
  await app.keyboard.press("PageDown");
  await expect(count).toHaveText("2 / 3");
  const top = (row) =>
    page
      .getByText(`Row ${row}.`)
      .evaluate((cell) => cell.parentElement.getBoundingClientRect().top);
  // Row 21 starts the second sheet at its top margin; the rows of the
  // sheets before and after are cut away.
  await expect.poll(async () => mm(await top(21))).toBe(25);
  for (const [row, shown] of [
    [20, false],
    [21, true],
    [40, true],
    [41, false],
  ])
    expect(await onSheet(page.getByText(`Row ${row}.`))).toBe(shown);
  // Printed from its own address, where only Chromium writes a PDF.
  if (browserName !== "chromium") return;
  const alone = await app.context().newPage();
  await alone.goto(
    await files.getByRole("link", { name: "Open alone" }).getAttribute("href"),
  );
  const pdf = await alone.pdf({ preferCSSPageSize: true });
  expect(pdf.toString("latin1").match(/\/Type\s*\/Page\b/g)).toHaveLength(3);
});

test("find in a document is counted by its pages and steps to the page of what it found", async ({
  page: app,
  session,
  host,
}) => {
  const { files, page, count } = await openMemo(app, session, host.project);
  const pages = MEMO_PAGES;
  await expect(count).toHaveText(`1 / ${pages}`);
  await files.getByRole("button", { name: "Find" }).click();
  const bar = files.getByRole("search", { name: "Find in this document" });
  const field = bar.getByRole("searchbox");
  await field.fill("OF THE MEMO");
  await expect(bar).toContainText("1 of 120");
  await expect(count).toHaveText(`1 / ${pages}`);
  const marks = (name) =>
    page
      .locator("html")
      .evaluate((_, name) => CSS.highlights.get(name)?.size ?? 0, name);
  // The current one, as the page marks it.
  const current = () =>
    page
      .locator("html")
      .evaluate(() =>
        [...(CSS.highlights.get("uagent-found") ?? [])]
          .map((range) => range.startContainer.textContent)
          .join(),
      );
  await expect.poll(() => marks("uagent-find")).toBe(120);
  await expect.poll(current).toBe("Paragraph 1 of the memo.");
  // The one before the first is the last, on the last page.
  await field.press("Shift+Enter");
  await expect(bar).toContainText("120 of 120");
  await expect(count).toHaveText(`${pages} / ${pages}`);
  await expect(page.getByText("Paragraph 120 of the memo.")).toBeInViewport();
  await expect.poll(current).toBe("Paragraph 120 of the memo.");
  // Past the last comes the first; the 14th is the second of page 2.
  for (let step = 0; step < 14; step++) await field.press("Enter");
  await expect(bar).toContainText("14 of 120");
  await expect(count).toHaveText(`2 / ${pages}`);
  await expect.poll(current).toBe("Paragraph 14 of the memo.");
  // New words start at their first occurrence.
  await field.fill("revenue grew");
  await expect(bar).toContainText("1 of 1");
  await expect(count).toHaveText(`1 / ${pages}`);
  await field.fill("wombat");
  await expect(bar).toContainText("No matches");
  await bar.getByRole("button", { name: "Close find" }).click();
  await expect.poll(() => marks("uagent-find")).toBe(0);
});

test("a document exports Markdown, and a block edited on its third page is saved to its file", async ({
  page: app,
  session,
  host,
}) => {
  const { files, memo, page, count } = await openMemo(
    app,
    session,
    host.project,
  );
  await expect(page.getByRole("heading", { name: "Q3 memo" })).toBeVisible();

  // Markdown, from the page itself.
  const [download] = await Promise.all([
    app.waitForEvent("download"),
    files
      .getByRole("button", { name: "Export" })
      .click()
      .then(() => app.getByRole("menuitem", { name: "Markdown" }).click()),
  ]);
  expect(download.suggestedFilename()).toBe("memo.md");
  const { readFile } = await import("node:fs/promises");
  const text = await readFile(await download.path(), "utf8");
  expect(text).toContain("# Q3 memo\n\nRevenue grew.\n\n- One\n- Two");
  expect(text).toContain("| Ask | When |\n| --- | --- |\n| Hire | Nov |");

  // Edit: a block typed in place is saved to the file, nothing else changes,
  // and the document drawn again shows the page it showed.
  await app.keyboard.press("PageDown");
  await app.keyboard.press("PageDown");
  await expect(count).toHaveText(`3 / ${MEMO_PAGES}`);
  await files.getByRole("button", { name: "Edit" }).click();
  const paragraph = page.getByText(/^Paragraph 25 of the memo\./);
  const beside = page.getByText("Paragraph 26 of the memo.");
  await paragraph.click();
  await app.keyboard.press("End");
  await app.keyboard.type(" Churn held flat.");
  await beside.click();
  await expect(files.locator(".page-tools .notice")).toHaveText("Saved");
  const saved = await readFile(memo, "utf8");
  expect(saved).toContain("<p>Paragraph 25 of the memo. Churn held flat.</p>");
  await expect(paragraph).toHaveText(/Churn held flat\.$/);
  // The legend's picture of the page is of the file as saved.
  await expect(
    files
      .locator(".page-legend-item")
      .nth(2)
      .locator("iframe:not([inert])")
      .contentFrame()
      .getByText(/Churn held flat\.$/),
  ).toBeAttached();
  await expect(count).toHaveText(`3 / ${MEMO_PAGES}`);
  expect(saved).toContain("<td>Hire</td>");
  expect(
    saved.startsWith("<!doctype html><html><head><title>Memo</title>"),
  ).toBe(true);

  // The agent rewrote that block meanwhile: the edit is refused, not merged.
  await writeFile(memo, saved.replace("Churn held flat.", "Churn fell."));
  await paragraph.click();
  await app.keyboard.press("End");
  await app.keyboard.type(" Again.");
  await beside.click();
  await expect(files.locator(".page-tools .notice")).toHaveText(
    /The document changed/,
  );
  expect(await readFile(memo, "utf8")).toContain("Churn fell.");
});
