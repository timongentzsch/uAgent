// A design's pages open as a canvas in the Files pane, a deck's slides and a
// document's pages in a viewer with a legend: each page live in a sandboxed
// frame that reaches neither the app nor the host's API.
import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { test, expect, headerAction, selectProbeModel } from "./fixtures.js";

const page = (title, body, size = "1440x900") =>
  `<!doctype html><html><head><title>${title}</title>` +
  `<meta name="artboard" content="${size}"></head><body>${body}</body></html>`;

async function design(project) {
  const dir = join(project, "design");
  await mkdir(dir);
  await writeFile(
    join(dir, "01-hero-a.html"),
    page("Hero A", "<h1>Alpha</h1>"),
  );
  await writeFile(join(dir, "01-hero-b.html"), page("Hero B", "<h1>Beta</h1>"));
  await writeFile(
    join(dir, "cover.html"),
    // An error as the page would raise it, without failing the test's page.
    page(
      "Cover",
      '<p>Cover</p><script>dispatchEvent(new ErrorEvent("error", { message: "ReferenceError: missing is not defined", lineno: 3 }))</script>',
      "390x844",
    ),
  );
}

async function openCanvas(app) {
  await headerAction(app, "Files");
  const files = app.getByRole("region", { name: "Files" });
  await files.getByRole("button", { name: "design", exact: true }).click();
  await files.getByRole("button", { name: "Open", exact: true }).click();
  return files;
}

test("a design folder opens as a canvas of sandboxed pages", async ({
  page: app,
  session,
  host,
}) => {
  await design(host.project);
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  const files = await openCanvas(app);
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
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  await openCanvas(app);
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
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  const files = await openCanvas(app);
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
  await app.goto(`/#session=${session.id}`);
  await headerAction(app, "Files");
  const files = app.getByRole("dialog", { name: "Files" });
  await files.getByRole("button", { name: "design", exact: true }).click();
  await files.getByRole("button", { name: "Open", exact: true }).click();
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
  await app.setViewportSize({ width: 390, height: 844 });
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
  const dir = join(host.project, "design");
  await mkdir(dir);
  const tweaks = JSON.stringify({
    "--gap": { editor: "range", default: 8, min: 0, max: 32, unit: "px" },
    variant: {
      editor: "enum",
      default: "split",
      options: ["split", "stacked"],
    },
  });
  await writeFile(
    join(dir, "hero-a.html"),
    page(
      "Hero A",
      `<script type="application/json" id="tweaks">${tweaks}</script><main><h1>Alpha headline</h1></main>`,
    ),
  );
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  await headerAction(app, "Files");
  const files = app.getByRole("region", { name: "Files" });
  await files.getByRole("button", { name: "design", exact: true }).click();
  // A design of one page has no canvas: Open shows the page, with no legend.
  await files.getByRole("button", { name: "Open", exact: true }).click();
  const frame = app.frameLocator('iframe[title="Hero A"]');
  await expect(frame.getByText("Alpha headline")).toBeVisible();
  await expect(files.locator(".canvas-page")).toHaveCount(0);
  await expect(files.getByRole("listbox")).toHaveCount(0);

  // Select picks an element; the comment goes into the draft, pane open.
  await files.getByRole("button", { name: "Select" }).click();
  await frame.getByText("Alpha headline").click();
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
  const system = join(host.project, "design", "system");
  await mkdir(system, { recursive: true });
  await writeFile(
    join(system, "tokens.css"),
    ':root, [data-theme="light"] {\n  --page: #ffffff;\n}\n[data-theme="dark"] {\n  --page: #000000;\n}\n',
  );
  // The book the agent writes: A4 wide, as tall as its pages.
  await writeFile(
    join(system, "book.html"),
    page(
      "Brand book",
      '<section style="height:297mm;background:var(--page)"><h1>Brand</h1></section>',
      "794",
    ).replace(
      "</head>",
      '<meta name="print" content="flow a4"><link rel="stylesheet" href="tokens.css"></head>',
    ),
  );
  await mkdir(join(host.project, "design", "landing"));
  await writeFile(
    join(host.project, "design", "landing", "hero-a.html"),
    page("Hero A", "<h1>Alpha</h1>").replace(
      "</head>",
      '<link rel="stylesheet" href="../system/tokens.css"></head>',
    ),
  );
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  await headerAction(app, "Files");
  const files = app.getByRole("region", { name: "Files" });
  await files.getByRole("button", { name: "design", exact: true }).click();
  await files.getByRole("button", { name: "system", exact: true }).click();
  // Open on a design system is its book, never a canvas.
  await files.getByRole("button", { name: "Open", exact: true }).click();
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
  const system = join(host.project, "design", "system");
  await mkdir(system, { recursive: true });
  await writeFile(join(system, "tokens.css"), ":root{--page:#fff}");
  await writeFile(
    join(system, "book.html"),
    page(
      "Brand book",
      '<section style="height:297mm"><h1>Brand</h1></section>',
      "794",
    ),
  );
  await app.setViewportSize({ width: 390, height: 844 });
  await app.goto(`/#session=${session.id}`);
  await headerAction(app, "Files");
  const files = app.getByRole("dialog", { name: "Files" });
  await files.getByRole("button", { name: "design", exact: true }).click();
  await files.getByRole("button", { name: "system", exact: true }).click();
  await files.getByRole("button", { name: "book.html", exact: true }).click();
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
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  await headerAction(app, "Files");
  const files = app.getByRole("region", { name: "Files" });
  await files.getByRole("button", { name: "design", exact: true }).click();
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
  const dir = join(host.project, "design");
  await mkdir(dir);
  await writeFile(join(dir, "home.html"), page("Home", "<p>Short</p>", "1440"));
  await writeFile(join(dir, "hero.html"), page("Hero", "<h1>Hero</h1>"));
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  const files = await openCanvas(app);
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

test("an opened page zooms as the canvas does, from the controls and over the page", async ({
  page: app,
  session,
  host,
}) => {
  await design(host.project);
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  await headerAction(app, "Files");
  const files = app.getByRole("region", { name: "Files" });
  await files.getByRole("button", { name: "design", exact: true }).click();
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
  const dir = join(host.project, "design", "book");
  await mkdir(dir, { recursive: true });
  await writeFile(
    join(dir, "book.html"),
    page(
      "Book",
      '<style>section{height:300px;overflow:hidden}</style><section><div style="height:500px">Long</div></section><img src="missing.png" alt="">',
      "794",
    ),
  );
  await writeFile(join(dir, "02-cover.html"), page("Cover", "<p>ok</p>"));
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  await headerAction(app, "Files");
  const files = app.getByRole("region", { name: "Files" });
  await files.getByRole("button", { name: "design", exact: true }).click();
  await files.getByRole("button", { name: "book", exact: true }).click();
  await files.getByRole("button", { name: "Open", exact: true }).click();
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
  const dir = join(host.project, "design", "checks");
  await mkdir(dir, { recursive: true });
  await writeFile(
    join(dir, "01-slide.html"),
    page(
      "Slide",
      '<p style="position:absolute;top:100px;left:100px;margin:0;font-size:40px">Revenue grew</p>' +
        '<p style="position:absolute;top:110px;left:120px;margin:0;font-size:40px">Footer note</p>' +
        '<div style="position:absolute;top:1040px;left:100px;overflow:hidden;height:20px"><p style="margin:0;font-size:40px">Cut off</p></div>' +
        "<aside>Notes stay out of the check.</aside>",
      "1920x1080 slide",
    ),
  );
  // A5 with no artboard: as wide as its paper, and a figure taller than it.
  await writeFile(
    join(dir, "memo.html"),
    '<!doctype html><html><head><title>Memo</title><meta name="print" content="flow a5"></head>' +
      '<body style="margin:0"><figure style="height:1000px;margin:0">tall</figure></body></html>',
  );
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  await headerAction(app, "Files");
  const files = app.getByRole("region", { name: "Files" });
  await files.getByRole("button", { name: "design", exact: true }).click();
  await files.getByRole("button", { name: "checks", exact: true }).click();
  // The one slide is a deck; the document beside it is its own.
  await files.getByRole("button", { name: "Open", exact: true }).click();
  await expect(files.getByText("1 page failed to run")).toBeVisible();
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  const draft = app.locator("#prompt");
  await expect(draft).toHaveValue(
    /`design\/checks\/01-slide\.html` threw: “Cut off” runs past the slide's 1920x1080 edge.*“Revenue grew” overlaps “Footer note”/,
  );
  await files.getByRole("button", { name: "Back to the list" }).click();
  await files.getByRole("button", { name: "memo.html", exact: true }).click();
  await expect(
    files.frameLocator(".page-stage iframe").getByText("tall"),
  ).toBeVisible();
  await expect(files.getByText("1 page failed to run")).toBeVisible();
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  await expect(draft).toHaveValue(
    /`design\/checks\/memo\.html` threw: a figure is 1000px tall, more than a page \(794px\)/,
  );
});

// A slide of a deck: its title as its heading, its notes in its last aside.
const slide = (title, notes, body = "") =>
  page(
    title,
    `<h1>${title}</h1>${body}${notes ? `<aside>${notes}</aside>` : ""}`,
    "1920x1080 slide",
  );

// A deck in decks/talk, opened from the tree: the pane, its legend, the
// stage's page and the counter.
async function openDeck(app, session, project, slides) {
  const dir = join(project, "decks", "talk");
  await mkdir(dir, { recursive: true });
  for (const [name, html] of Object.entries(slides))
    await writeFile(join(dir, name), html);
  await app.goto(`/#session=${session.id}`);
  await headerAction(app, "Files");
  const files = app.getByRole(
    app.viewportSize().width < 600 ? "dialog" : "region",
    { name: "Files" },
  );
  await files.getByRole("button", { name: "decks", exact: true }).click();
  await files.getByRole("button", { name: "talk", exact: true }).click();
  await files.getByRole("button", { name: "Open", exact: true }).click();
  return {
    files,
    legend: files.getByRole("listbox", { name: "Slides" }),
    stage: files.frameLocator(".page-stage iframe"),
    count: files.getByRole("group", { name: "Step" }).locator("output"),
  };
}

test("a deck opens in slide order, presents with its notes, and the presenter's window follows", async ({
  page: app,
  session,
  host,
}) => {
  await app.setViewportSize({ width: 1440, height: 900 });
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
  await app.setViewportSize({ width: 1440, height: 900 });
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

test("a long deck's legend draws only the slides near its view, and keeps the current one in it", async ({
  page: app,
  session,
  host,
}) => {
  await app.setViewportSize({ width: 1440, height: 900 });
  const names = Array.from({ length: 40 }, (_, at) =>
    String(at + 1).padStart(2, "0"),
  );
  const { legend, count } = await openDeck(
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
});

test("on a phone a deck's legend is a filmstrip under the stage", async ({
  page: app,
  session,
  host,
}) => {
  await app.setViewportSize({ width: 390, height: 844 });
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
  await app.setViewportSize({ width: 1440, height: 900 });
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
  await app.setViewportSize({ width: 1440, height: 900 });
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
  await app.setViewportSize({ width: 1440, height: 900 });
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
  const dir = join(host.project, "design");
  await mkdir(dir);
  for (const [name, title] of [
    ["01-home", "Home"],
    ["02-pricing", "Pricing"],
    ["03-about", "About"],
  ])
    await writeFile(
      join(dir, `${name}.html`),
      page(title, `<h1>${title}</h1>`),
    );
  await writeFile(
    join(dir, "terms.html"),
    '<!doctype html><html><head><title>Terms</title><meta name="print" content="flow a4"></head><body><h1>Terms</h1></body></html>',
  );
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  const files = await openCanvas(app);
  // The canvas shows all four.
  await expect(files.locator(".canvas-page")).toHaveCount(4);
  await files.getByRole("button", { name: "Open Pricing" }).click();
  const legend = files.getByRole("listbox", { name: "Pages" });
  const stage = files.frameLocator(".page-stage iframe");
  const count = files.getByRole("group", { name: "Step" }).locator("output");
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
  await files.getByRole("button", { name: "Open Terms" }).click();
  await expect(stage.getByRole("heading", { name: "Terms" })).toBeVisible();
  await expect(files.getByRole("listbox")).toHaveCount(0);
});

test("find follows what the agent rewrites while it is open", async ({
  page: app,
  session,
  host,
  command,
}) => {
  await design(host.project);
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  const files = await openCanvas(app);
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
    files
      .frameLocator(".page-stage iframe")
      .getByRole("heading", { name: "Rewritten" }),
  ).toBeVisible();
});

// docs/ends.html: three pages, the first nearly a full one and the other
// two short, so the last two lie together in the document's last view.
for (const [where, width, height] of [
  ["a phone, where the counter is the only stepper", 390, 844],
  ["a wide pane, by the legend too", 1440, 900],
])
  test(`a document's last pages are stepped to though it cannot scroll to them: on ${where}`, async ({
    page: app,
    session,
    host,
  }) => {
    const dir = join(host.project, "docs");
    await mkdir(dir, { recursive: true });
    await writeFile(
      join(dir, "ends.html"),
      '<!doctype html><html><head><title>Ends</title><meta name="print" content="flow a5"><style>*{margin:0}</style></head>' +
        '<body><p style="height:780px">One</p>' +
        '<h2 style="break-before:page;height:40px">Two</h2><p style="height:400px">second</p>' +
        '<h2 style="break-before:page;height:40px">Three</h2><p style="height:100px">third</p></body></html>',
    );
    await app.setViewportSize({ width, height });
    await app.goto(`/#session=${session.id}`);
    await headerAction(app, "Files");
    const phone = width < 600;
    const files = app.getByRole(phone ? "dialog" : "region", { name: "Files" });
    await files.getByRole("button", { name: "docs", exact: true }).click();
    await files.getByRole("button", { name: "ends.html", exact: true }).click();
    const count = files.getByRole("group", { name: "Step" }).locator("output");
    const page = files.frameLocator(".page-stage iframe").locator("html");
    const next = files.getByRole("button", { name: "Next page" });
    const previous = files.getByRole("button", { name: "Previous page" });
    const legend = files.getByRole("listbox", { name: "Pages" });
    await expect(count).toHaveText("Page 1 / 3");
    await expect(legend).toBeVisible({ visible: !phone });
    // Page 2 starts past where the document can scroll: it goes to its end.
    await next.click();
    await expect(count).toHaveText("Page 2 / 3");
    await expect
      .poll(() => page.evaluate(() => scrollY + innerHeight))
      .toBeGreaterThan(1358);
    // Scrolled by hand the choice is forgotten: the page at the top counts,
    // and at the end the last.
    await page.evaluate(() => scrollTo(0, 10));
    await expect(count).toHaveText("Page 1 / 3");
    await page.evaluate(() => scrollTo(0, 2000));
    await expect(count).toHaveText("Page 3 / 3");
    await expect(next).toBeDisabled();
    // From there Previous and Next need no scroll at all.
    await previous.click();
    await expect(count).toHaveText("Page 2 / 3");
    await next.click();
    await expect(count).toHaveText("Page 3 / 3");
    if (!phone) {
      await legend.getByRole("option", { name: "Page 2" }).click();
      await expect(count).toHaveText("Page 2 / 3");
      await expect(
        legend.getByRole("option", { name: "Page 2" }),
      ).toHaveAttribute("aria-selected", "true");
      await legend.getByRole("option", { name: "Page 3" }).click();
      await expect(count).toHaveText("Page 3 / 3");
    }
    await previous.click();
    await expect(count).toHaveText("Page 2 / 3");
    await previous.click();
    await expect(count).toHaveText("Page 1 / 3");
    await expect.poll(() => page.evaluate(() => scrollY)).toBe(0);
  });

// docs/memo.html, opened from the tree: an A5 document of ten parts, each
// starting a page and shorter than one, so it has ten pages at any width.
const MEMO_PAGES = 10;
async function openMemo(app, session, project) {
  const dir = join(project, "docs");
  await mkdir(dir, { recursive: true });
  const memo = join(dir, "memo.html");
  const parts = Array.from({ length: MEMO_PAGES }, (_, part) =>
    Array.from(
      { length: 12 },
      (_, at) =>
        `<p>Paragraph ${part * 12 + at + 1} of the memo, long enough to fill some of a page.</p>`,
    ).join(""),
  ).join('<h2 style="break-before:page">Next part</h2>');
  await writeFile(
    memo,
    '<!doctype html>\n<html><head><title>Memo</title><meta name="print" content="flow a5"></head>' +
      "<body><h1>Q3 memo</h1><p>Revenue grew.</p><ul><li>One</li><li>Two</li></ul>" +
      "<table><tr><th>Ask</th><th>When</th></tr><tr><td>Hire</td><td>Nov</td></tr></table>" +
      `${parts}</body></html>`,
  );
  await app.setViewportSize({ width: 1440, height: 900 });
  await app.goto(`/#session=${session.id}`);
  await headerAction(app, "Files");
  const files = app.getByRole("region", { name: "Files" });
  await files.getByRole("button", { name: "docs", exact: true }).click();
  await files.getByRole("button", { name: "memo.html", exact: true }).click();
  return { files, memo, page: files.frameLocator(".page-stage iframe") };
}

test("a document's legend is its pages on one picture; a click, the Page keys and its own scroll move through them", async ({
  page: app,
  session,
  host,
}) => {
  const { files, page } = await openMemo(app, session, host.project);
  const legend = files.getByRole("listbox", { name: "Pages" });
  const count = files.getByRole("group", { name: "Step" }).locator("output");
  const pages = MEMO_PAGES;
  await expect(count).toHaveText(`Page 1 / ${pages}`);
  // One frame of the whole document, a numbered area a page.
  await expect(legend.getByRole("option")).toHaveCount(pages);
  await expect(legend.locator("iframe")).toHaveCount(1);
  const shows = async (at) => {
    await expect(count).toHaveText(`Page ${at} / ${pages}`);
    await expect(
      legend.getByRole("option", { name: `Page ${at}`, exact: true }),
    ).toHaveAttribute("aria-selected", "true");
  };
  const scrolled = () => page.locator("html").evaluate(() => scrollY);
  await shows(1);
  await expect(
    files.getByRole("button", { name: "Previous page" }),
  ).toBeDisabled();
  // A click on a page scrolls the document to it, inside its frame.
  await legend.getByRole("option", { name: "Page 2", exact: true }).click();
  await shows(2);
  await expect.poll(scrolled).toBeGreaterThan(0);
  // The keys move by page, to the ends too; the last page is the end.
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
  await expect(page.getByText("Paragraph 13 of the memo")).not.toBeInViewport();
  // Scrolled by hand from there, the document says where it is, and the
  // next page's start is marked on the stage.
  await page.locator("html").evaluate(() => scrollTo(0, 0));
  await shows(1);
  await expect(files.locator(".page-break").first()).toHaveText("Page 2");
});

test("find in a document counts in the page itself and scrolls to what it found", async ({
  page: app,
  session,
  host,
}) => {
  const { files, page } = await openMemo(app, session, host.project);
  const count = files.getByRole("group", { name: "Step" }).locator("output");
  const pages = MEMO_PAGES;
  await expect(count).toHaveText(`Page 1 / ${pages}`);
  // What the page is asked to find, each as "words index".
  await page.locator("html").evaluate(() => {
    window.asked = [];
    addEventListener(
      "message",
      (event) =>
        event.data?.type === "find" &&
        window.asked.push(`${event.data.text} ${event.data.index}`),
    );
  });
  await files.getByRole("button", { name: "Find" }).click();
  const bar = files.getByRole("search", { name: "Find in this document" });
  const field = bar.getByRole("searchbox");
  await field.fill("OF THE MEMO");
  await expect(bar).toContainText("1 of 120");
  await expect(count).toHaveText(`Page 1 / ${pages}`);
  const marks = (name) =>
    page
      .locator("html")
      .evaluate((_, name) => CSS.highlights.get(name)?.size ?? 0, name);
  await expect.poll(() => marks("uagent-find")).toBe(120);
  // The one before the first is the last, on the last page.
  await field.press("Shift+Enter");
  await expect(bar).toContainText("120 of 120");
  await expect(count).toHaveText(`Page ${pages} / ${pages}`);
  await expect(page.getByText("Paragraph 120 of the memo")).toBeInViewport();
  // New words start at their first occurrence, and are asked for once.
  await field.fill("revenue grew");
  await expect(bar).toContainText("1 of 1");
  await expect(count).toHaveText(`Page 1 / ${pages}`);
  expect(await page.locator("html").evaluate(() => window.asked)).toEqual([
    "OF THE MEMO 0",
    "OF THE MEMO 119",
    "revenue grew 0",
  ]);
  await field.fill("wombat");
  await expect(bar).toContainText("No matches");
  await bar.getByRole("button", { name: "Close find" }).click();
  await expect.poll(() => marks("uagent-find")).toBe(0);
});

test("a document is edited in place, saved to its file, and exports Markdown", async ({
  page: app,
  session,
  host,
}) => {
  const { files, memo, page } = await openMemo(app, session, host.project);
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

  // Edit: a block typed in place is saved to the file, nothing else changes.
  await files.getByRole("button", { name: "Edit" }).click();
  const paragraph = page.getByText("Revenue grew.");
  await paragraph.click();
  await app.keyboard.press("End");
  await app.keyboard.type(" Churn held flat.");
  await page.getByRole("heading", { name: "Q3 memo" }).click();
  await expect(files.locator(".page-tools").getByRole("status")).toHaveText(
    "Saved",
  );
  const saved = await readFile(memo, "utf8");
  expect(saved).toContain("<p>Revenue grew. Churn held flat.</p>");
  expect(saved).toContain("<td>Hire</td>");
  expect(
    saved.startsWith("<!doctype html>\n<html><head><title>Memo</title>"),
  ).toBe(true);

  // The agent rewrote that block meanwhile: the edit is refused, not merged.
  await writeFile(memo, saved.replace("Churn held flat.", "Churn fell."));
  await paragraph.click();
  await app.keyboard.press("End");
  await app.keyboard.type(" Again.");
  await page.getByRole("heading", { name: "Q3 memo" }).click();
  await expect(files.locator(".page-tools").getByRole("status")).toHaveText(
    /The document changed/,
  );
  expect(await readFile(memo, "utf8")).toContain("Churn fell.");
});
