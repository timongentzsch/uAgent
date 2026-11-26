// Every popup has the same chrome: its title and Close on one bar as high as
// a bar is, Close's icon as far from the corner's two edges as the title is
// from the other side, Close in the same place, a body inset alike on every
// side, and all of it in the window.
import {
  test,
  expect,
  headerAction,
  openMore,
  openRawContext,
  showList,
} from "./fixtures.js";
import { chrome } from "./popups.js";

test.use({ places: true });

const PNG = {
  name: "sample.png",
  mimeType: "image/png",
  buffer: Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAAAAACRn/EaAAAADklEQVR4nGNoAAIGOAEALQ8GAa0C414AAAAASUVORK5CYII=",
    "base64",
  ),
};

// The browser's status, so its popup draws without a Chrome on the host.
const browserStatus = (page) =>
  page.route("**/api/browser/status", (route) =>
    route.fulfill({
      json: {
        ok: true,
        display: 1,
        generation: 1,
        profile_id: "default",
        profiles: [{ id: "default", name: "Default" }],
      },
    }),
  );

const fromMenu = async (page, name, more = false) => {
  await page
    .locator(".conversation-head")
    .getByRole("button", { name: "Conversation menu" })
    .click();
  if (more) await openMore(page);
  await page.getByRole("menuitem", { name, exact: true }).click();
};
const composer = (page, name) =>
  page.locator(".composer").getByRole("button", { name, exact: true }).click();

// Each popup by its name, and how it is opened from a conversation.
// The conversation's totals, the way to these two, show beside a wide input.
const WIDE = new Set(["Conversation statistics", "Raw context"]);
// The list is a drawer only where it does not fit beside the conversation.
const NARROW = new Set(["Conversations"]);
const POPUPS = {
  Conversations: showList,
  Settings: (page) => headerAction(page, "Settings"),
  Browser: (page) => headerAction(page, "Open browser"),
  "Conversation statistics": (page) =>
    composer(page, "Conversation statistics"),
  "Raw context": openRawContext,
  "Model and effort": (page) => composer(page, "Model and effort"),
  Approval: (page) => composer(page, "Approval"),
  Tools: (page) => fromMenu(page, "Tools", true),
  "Rename conversation": (page) => fromMenu(page, "Rename"),
  "Delete conversation?": (page) => fromMenu(page, "Delete", true),
  "sample.png": async (page) => {
    if (!(await page.locator(".composer .file-chip").count()))
      await page.locator('.composer input[type="file"]').setInputFiles(PNG);
    await page.getByRole("button", { name: "View sample.png" }).click();
  },
};

// A length a token names, in pixels, as this page resolves it.
const token = (page, name) =>
  page.evaluate((name) => {
    const probe = document.createElement("div");
    probe.style.width = `var(${name})`;
    document.body.append(probe);
    const width = probe.getBoundingClientRect().width;
    probe.remove();
    return Math.round(width * 10) / 10;
  }, name);

async function checkAll(page, session, safeTop = 0) {
  await browserStatus(page);
  await page.goto(`/#session=${session.id}`);
  await expect(page.getByLabel("Message or guidance")).toBeEnabled();
  // Desktop engines expose no iPhone system inset: it is set here.
  if (safeTop)
    await page.evaluate((top) => {
      const style = document.documentElement.style;
      style.setProperty("--safe-top", `${top}px`);
      style.setProperty("--safe-bottom", "34px");
    }, safeTop);
  const bar = await token(page, "--bar-min");
  const control = await token(page, "--control");
  const inset = await token(page, "--bar-text");
  const corners = new Set();
  const wide = (await page.viewportSize()).width > 600;
  for (const [name, open] of Object.entries(POPUPS)) {
    if (wide ? NARROW.has(name) : WIDE.has(name)) continue;
    await open(page);
    const dialog = page.getByRole("dialog", { name, exact: true });
    await expect(dialog).toBeVisible();
    // Opened: its entrance has played.
    await expect
      .poll(() => dialog.evaluate((node) => node.getAnimations().length))
      .toBe(0);
    const look = await chrome(page);
    const at = (value) => Math.round(value);
    expect
      .soft(
        [look.title, look.glyph.right, look.glyph.top, look.glyph.bottom].map(
          at,
        ),
        `${name}: the title's and Close's insets`,
      )
      .toEqual(Array(4).fill(at(inset)));
    expect.soft(look.header, `${name}: its header's height`).toBe(bar);
    expect
      .soft([look.close.width, look.close.height], `${name}: Close's size`)
      .toEqual([control, control]);
    expect.soft(look.gutter, `${name}: room kept for a scrollbar`).toBe(0);
    expect.soft(look.outside, `${name}: in the window`).toBe(false);
    expect
      .soft(look.headerTop, `${name}: below the safe area`)
      .toBeGreaterThanOrEqual(safeTop);
    if (look.body && look.body[0] !== "0px")
      expect
        .soft(
          look.body.map(
            (side) => Math.round(Number.parseFloat(side) * 10) / 10,
          ),
          `${name}: its body's inset`,
        )
        .toEqual(Array(4).fill(inset));
    const pad = await token(page, "--bar-pad");
    for (const [label, top, bottom] of look.controls)
      expect
        .soft([top, bottom], `${name}: ${label} on the bar`)
        .toEqual([pad, pad]);
    corners.add(JSON.stringify(look.close));
    await page.keyboard.press("Escape");
    await expect(dialog).toBeHidden();
  }
  expect([...corners], "Close stands in one place in every popup").toHaveLength(
    1,
  );
}

for (const [width, height] of [
  [1280, 800],
  [1440, 900],
])
  test(`every popup's chrome at ${width}px`, async ({ page, session }) => {
    await page.setViewportSize({ width, height });
    await checkAll(page, session);
  });

test.describe("on a phone", () => {
  test.use({
    isMobile: true,
    hasTouch: true,
    viewport: { width: 390, height: 844 },
  });

  test("every popup's chrome, inside the safe area", async ({
    page,
    session,
  }) => {
    await checkAll(page, session, 47);
  });
});
