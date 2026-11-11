// A reload looks like the app, not a page loading: its frame paints before
// any script runs, the conversation fills it in place, and nothing jumps.
import { test, expect } from "./fixtures.js";

// What a page route holds or refuses must reach it: WebKit sends a page's
// requests past routes once a service worker controls it.
test.use({ serviceWorkers: "block" });

const box = (locator) =>
  locator.evaluate((node) => {
    const { x, y, width, height } = node.getBoundingClientRect();
    return { x, y, width, height };
  });
// Where a list sits and how wide it is; its height is its rows'.
const placed = (a, b) =>
  ["x", "y", "width"].every((key) => Math.abs(a[key] - b[key]) <= 1);
const near = (a, b) =>
  ["x", "y", "width", "height"].every((key) => Math.abs(a[key] - b[key]) <= 1);

for (const [name, viewport] of [
  ["phone", { width: 390, height: 844 }],
  ["desktop", { width: 1280, height: 800 }],
])
  test(`the frame paints before any script, where the app then draws (${name})`, async ({
    page,
    session,
  }) => {
    await page.setViewportSize(viewport);
    const entry = /\/assets\/index-[^/]*\.js$/;
    await page.route(entry, (route) => route.abort());
    await page.goto(`/#session=${session.id}`);
    const frame = page.locator(".boot-frame");
    const head = frame.locator(".conversation-head");
    await expect(head).toBeVisible();
    await expect(frame.locator(".sidebar")).toBeVisible({
      visible: name === "desktop",
    });
    const before = {
      head: await box(head),
      sidebar: name === "desktop" ? await box(frame.locator(".sidebar")) : null,
    };
    await page.unroute(entry);
    await page.reload();
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    await expect(page.locator(".boot-frame")).toHaveCount(0);
    await expect(page.locator(".shell")).toHaveCount(1);
    expect(
      near(before.head, await box(page.locator(".conversation-head"))),
    ).toBe(true);
    if (before.sidebar)
      expect(
        near(before.sidebar, await box(page.locator("aside.sidebar"))),
      ).toBe(true);
  });

test("the open conversation is asked for with the list, not after it", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await expect(page.getByLabel("Message or guidance")).toBeEnabled();
  let release;
  const held = new Promise((resolve) => (release = resolve));
  await page.route(/\/api\/sessions(\?refresh=1)?$/, async (route) => {
    await held;
    await route.continue();
  });
  const snapshot = page.waitForRequest((request) =>
    request.url().endsWith(`/api/sessions/${session.id}`),
  );
  await page.reload();
  await snapshot;
  release();
  await expect(page.getByLabel("Message or guidance")).toBeEnabled();
});

test("a host that cannot be reached keeps the app, not the pairing form", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await expect(page.getByLabel("Message or guidance")).toBeEnabled();
  let failing = true;
  await page.route(/\/api\/sessions(\?refresh=1)?$/, (route) =>
    failing
      ? route.fulfill({ status: 503, body: "unavailable" })
      : route.continue(),
  );
  await page.reload();
  await expect(page.locator(".conversation-head")).toBeVisible();
  await expect(
    page.getByRole("heading", { name: "Your local coding agent." }),
  ).toHaveCount(0);
  failing = false;
  await expect(page.getByLabel("Message or guidance")).toBeEnabled({
    timeout: 20_000,
  });
});

test("a reload comes back to the page it left", async ({ page, session }) => {
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Library", exact: true }).click();
  await expect(page.locator(".management-toolbar")).toBeVisible();
  await page.reload();
  await expect(page.locator(".management-toolbar")).toBeVisible();
  await expect(
    page.getByRole("button", { name: "Library", exact: true }),
  ).toHaveAttribute("aria-current", "page");
});

test("Scheduled draws its list from samples while its tasks load", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Scheduled", exact: true }).click();
  const list = page.locator(".management-list");
  await expect(list).toBeVisible();
  const loaded = await box(list);
  let release;
  const held = new Promise((resolve) => (release = resolve));
  await page.route(/\/api\/sessions(\?refresh=1)?$/, async (route) => {
    await held;
    await route.continue();
  });
  await page.reload();
  await expect(page.locator(".placeholder .management-list")).toBeVisible();
  await expect(page.locator(".loading-surface")).toHaveCount(0);
  expect(placed(loaded, await box(page.locator(".management-list")))).toBe(
    true,
  );
  release();
  await expect(page.locator(".placeholder")).toHaveCount(0);
  expect(placed(loaded, await box(list))).toBe(true);
});

test("code that arrives within the delay shows no spinner", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await expect(page.getByLabel("Message or guidance")).toBeEnabled();
  await page.evaluate(() => {
    globalThis.spinners = 0;
    new MutationObserver(() => {
      globalThis.spinners +=
        document.querySelectorAll(".loading-indicator").length;
    }).observe(document.body, { childList: true, subtree: true });
  });
  await page.keyboard.press("ControlOrMeta+K");
  await expect(page.getByRole("dialog")).toBeVisible();
  expect(await page.evaluate(() => globalThis.spinners)).toBe(0);
});
