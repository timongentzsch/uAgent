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
  await writeFile(join(dir, "hero-a.html"), page("Hero A", "<h1>Alpha</h1>"));
  await writeFile(join(dir, "hero-b.html"), page("Hero B", "<h1>Beta</h1>"));
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
  // Rows by name prefix: the pages without one first, then "hero".
  await expect(files.getByRole("heading", { name: "hero" })).toBeVisible();
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

test("on a phone the canvas rows scroll sideways in the sheet", async ({
  page: app,
  session,
  host,
}) => {
  await design(host.project);
  await app.setViewportSize({ width: 390, height: 844 });
  await app.goto(`/#session=${session.id}`);
  const files = await openCanvas(app);
  const strip = files.locator(".canvas-strip").last();
  await expect(strip.locator(".canvas-page")).toHaveCount(2);
  expect(
    await strip.evaluate(
      (element) => element.scrollWidth > element.clientWidth,
    ),
  ).toBe(true);
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
  const files = await openCanvas(app);
  await files.getByRole("button", { name: "Open Hero A" }).click();
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

  // PDF opens the page alone, which prints itself.
  await expect(
    files.getByRole("link", { name: "Print or save as PDF" }),
  ).toHaveAttribute(
    "href",
    /\/preview\/[0-9a-f]{48}\/design\/hero-a\.html\?v=[\d.]+#print$/,
  );
});
