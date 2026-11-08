// A folder of HTML files opens as a canvas in the Files pane: each page live
// in a sandboxed frame that reaches neither the app nor the host's API.
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
  await files.getByRole("button", { name: "Canvas" }).click();
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
  const plane = files.locator(".canvas-plane");
  await expect(
    app.frameLocator('iframe[title="Hero A"]').getByText("Alpha"),
  ).toBeVisible();
  const before = await level.textContent();
  // A trackpad pinch arrives as the wheel with Ctrl.
  const stage = files.locator(".canvas-stage");
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
  await files.getByRole("button", { name: "Canvas" }).click();
  await expect(
    app.frameLocator('iframe[title="Hero A"]').getByText("Alpha"),
  ).toBeVisible();
  const level = files.locator(".zoom-controls output");
  const box = await files.locator(".canvas-stage").boundingBox();
  return {
    files,
    stage: files.locator(".canvas-stage"),
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
    const plane = files.locator(".canvas-plane");
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
  // A folder of one page has no canvas: its page is a row.
  await expect(files.getByRole("button", { name: "Canvas" })).toHaveCount(0);
  await files.getByRole("button", { name: "hero-a.html", exact: true }).click();
  const frame = app.frameLocator('iframe[title="Hero A"]');
  await expect(frame.getByText("Alpha headline")).toBeVisible();

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

  // Pictures need the agent's browser, which this host has not.
  await expect(
    files.getByRole("link", { name: "Download as PNG" }),
  ).toHaveCount(0);
  // PDF opens the page alone, which prints itself.
  await expect(
    files.getByRole("link", { name: "Print or save as PDF" }),
  ).toHaveAttribute(
    "href",
    /\/preview\/[0-9a-f]{48}\/design\/hero-a\.html\?v=[\d.]+#print$/,
  );
});

test("a design system's brand book is a page, and its themes are under Tweaks", async ({
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
  // Its one page is a row, not a canvas.
  await files.getByRole("button", { name: "book.html", exact: true }).click();
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
  await expect(
    files.getByRole("link", { name: "Print or save as PDF" }),
  ).toBeVisible();

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
  await files.getByRole("button", { name: "Canvas" }).click();
  await files.getByRole("button", { name: "Open Hero B" }).click();
  await files.getByRole("button", { name: "Back to the canvas" }).click();
  await expect(files.locator(".canvas-page")).toHaveCount(3);
  // Leaving the canvas gives the list back.
  await files.getByRole("button", { name: "Back to the list" }).click();
  await expect(
    files.getByRole("button", { name: "cover.html", exact: true }),
  ).toBeVisible();
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
  await files.getByRole("button", { name: "Canvas" }).click();
  await expect(files.getByText("1 page failed to run")).toBeVisible();
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    /`design\/book\/book\.html` threw: .*missing\.png did not load.*page 1 runs 200px past its height/,
  );
});
