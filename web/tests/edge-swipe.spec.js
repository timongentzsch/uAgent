// A swipe from the left edge is the app's own back on a phone: it closes the
// topmost layer, or opens the conversations drawer where none is open, and
// the touch is cancelled so Safari's own back swipe never starts. Touches are
// dispatched as events on what is under the finger, the one way both browsers
// of the suite take them.
import { test, expect, headerAction, online, showList } from "./fixtures.js";

test.use({
  isMobile: true,
  hasTouch: true,
  viewport: { width: 390, height: 844 },
});

const edge = { x: 6, y: 400 };

// A finger from the edge across the screen; whether the page cancelled its start,
// which is what keeps Safari from starting its back swipe. WebKit builds no
// Touch, so the events are plain ones carrying the touch list the page reads.
const swipe = (page, on, start = edge) =>
  page.evaluate(
    ([from, to, selector]) => {
      const target = selector
        ? document.querySelector(selector)
        : document.elementFromPoint(from.x, from.y);
      const send = (type, point) => {
        const event = new Event(type, { bubbles: true, cancelable: true });
        const touches = point
          ? [{ identifier: 1, clientX: point.x, clientY: point.y }]
          : [];
        Object.defineProperty(event, "touches", { value: touches });
        target.dispatchEvent(event);
        return event.defaultPrevented;
      };
      const cancelled = send("touchstart", from);
      for (let step = 1; step <= 6; step++)
        send("touchmove", {
          x: from.x + ((to.x - from.x) * step) / 6,
          y: from.y + ((to.y - from.y) * step) / 6,
        });
      send("touchend");
      return cancelled;
    },
    [start, { x: start.x + 154, y: start.y + 4 }, on],
  );
const history = (page) =>
  page.evaluate(() => ({
    length: window.history.length,
    layer: window.history.state?.layer ?? 0,
  }));
const drawer = (page) => page.getByRole("dialog", { name: "Conversations" });
const dialog = (page, name) => page.getByRole("dialog", { name });

// Each screen that is a layer: how it is opened, and how it is told apart.
const screens = {
  "a bottom sheet": [
    (page) =>
      page
        .getByRole("button", { name: "Conversation statistics", exact: true })
        .click(),
    (page) => dialog(page, "Conversation statistics"),
  ],
  Settings: [
    (page) => headerAction(page, "Settings"),
    (page) => dialog(page, "Settings"),
  ],
  Files: [
    (page) => headerAction(page, "Files"),
    (page) => dialog(page, "Files"),
  ],
};

test.beforeEach(async ({ page, session }) => {
  await page.goto(`/#session=${session.id}`);
  await online(page);
});

test("with nothing open it opens the drawer, and Safari's own swipe is cancelled", async ({
  page,
}) => {
  expect(await swipe(page)).toBe(true);
  await expect(drawer(page)).toBeVisible();
});

for (const [name, [show, layer]] of Object.entries(screens))
  test(`on ${name} it closes that layer like its Close, and Safari's swipe is cancelled`, async ({
    page,
  }) => {
    await show(page);
    await expect(layer(page)).toBeVisible();
    const opened = await history(page);
    expect(await swipe(page)).toBe(true);
    await expect(layer(page)).toHaveCount(0);
    await expect(drawer(page)).toHaveCount(0);
    // As Close leaves it: the layer's entry stepped back from.
    await expect
      .poll(() => history(page))
      .toEqual({ length: opened.length, layer: 0 });
  });

test("a pane of Settings closes first, then Settings", async ({ page }) => {
  await headerAction(page, "Settings");
  const settings = dialog(page, "Settings");
  await settings.getByRole("button").nth(1).click();
  const picked = await history(page);
  expect(picked.layer).toBe(2);
  expect(await swipe(page)).toBe(true);
  await expect.poll(() => history(page)).toMatchObject({ layer: 1 });
  await expect(settings).toBeVisible();
  expect(await swipe(page)).toBe(true);
  await expect(settings).toHaveCount(0);
  await expect.poll(() => history(page)).toMatchObject({ layer: 0 });
});

test("a page of the list returns to the conversation", async ({ page }) => {
  await showList(page);
  await drawer(page).getByRole("button", { name: "Library" }).click();
  await expect(page.getByRole("heading", { name: "Library" })).toBeVisible();
  expect(await swipe(page)).toBe(true);
  await expect(page.getByRole("heading", { name: "Library" })).toHaveCount(0);
  await expect(drawer(page)).toHaveCount(0);
  await expect.poll(() => history(page)).toMatchObject({ layer: 0 });
});

test("the drawer holds against a swipe from the side it came from", async ({
  page,
}) => {
  await showList(page);
  const opened = await history(page);
  expect(await swipe(page)).toBe(true);
  await expect(drawer(page)).toBeVisible();
  expect(await history(page)).toEqual(opened);
});

test("the Terminal keeps its screen's touches, and its tab row's", async ({
  page,
}) => {
  await headerAction(page, "Terminal");
  const terminal = dialog(page, "Terminal");
  await expect(terminal.locator(".terminal-view")).toBeVisible();
  expect(await swipe(page, ".terminal-view")).toBe(false);
  await expect(terminal).toBeVisible();
  // The tab row owns its horizontal gesture even when its first tab fits.
  const sheet = await terminal.boundingBox();
  expect(await swipe(page, undefined, { x: 6, y: sheet.y + 20 })).toBe(false);
  await expect(terminal).toBeVisible();
});

test("a slider, a scrolling line and what takes its own touches keep them", async ({
  page,
}) => {
  const sheet = dialog(page, "Conversation statistics");
  await page
    .getByRole("button", { name: "Conversation statistics", exact: true })
    .click();
  await expect(sheet).toBeVisible();
  await sheet.evaluate((node) => {
    const keep = {
      slider: '<div role="slider" tabindex="0">slider</div>',
      line: `<div style="overflow-x: auto; width: 100px"><div style="width: 400px">a long line</div></div>`,
      own: '<div style="touch-action: none">its own touches</div>',
    };
    for (const [name, html] of Object.entries(keep)) {
      const part = document.createElement("div");
      part.id = name;
      part.innerHTML = html;
      node.append(part);
    }
  });
  for (const name of ["slider", "line", "own"]) {
    await swipe(page, `#${name} > *`);
    await expect(sheet).toBeVisible();
  }
});

// The composer's field and the home screen's start box reach to within a few
// pixels of the edge: a finger there is a tap, and a swipe from it goes back
// as anywhere else.
test("a swipe from the composer's field opens the drawer, and a tap in its first pixels focuses it", async ({
  page,
}) => {
  const field = page.locator(".composer textarea");
  const box = await field.boundingBox();
  expect(box.x).toBeLessThan(24);
  await page.touchscreen.tap(box.x + 2, box.y + box.height / 2);
  await expect(field).toBeFocused();
  await field.blur();
  expect(await swipe(page, ".composer textarea")).toBe(false);
  await expect(drawer(page)).toBeVisible();
});

test("a swipe from the home screen's start box opens the drawer, and a tap in its first pixels focuses it", async ({
  page,
}) => {
  await page.goto("/");
  const field = page.getByLabel("Start a conversation");
  await expect(field).toBeEnabled();
  const box = await field.boundingBox();
  expect(box.x).toBeLessThan(24);
  await page.touchscreen.tap(box.x + 2, box.y + box.height / 2);
  await expect(field).toBeFocused();
  await field.blur();
  expect(await swipe(page, ".start-field textarea")).toBe(false);
  await expect(drawer(page)).toBeVisible();
});
