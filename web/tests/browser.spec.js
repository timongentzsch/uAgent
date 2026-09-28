import { test, expect } from "./fixtures.js";

const profiles = {
  profile_id: "default",
  profiles: [{ id: "default", name: "Default" }],
};
// Serves a status the test can change; the panel polls it.
const browserStatus = async (page, initial) => {
  const status = {
    ok: true,
    display: 1,
    generation: 1,
    ...profiles,
    ...initial,
  };
  await page.route("**/api/browser/status", (route) =>
    route.fulfill({ json: status }),
  );
  return status;
};
const openBrowser = async (page, session) => {
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Open browser" }).click();
  return page.getByRole("dialog", { name: "Browser", exact: true });
};
const rect = async (locator) => {
  const box = await locator.boundingBox();
  return box && [box.x, box.y, box.width, box.height].map(Math.round);
};

test("browser panel fits the viewport on a desktop", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await browserStatus(page, { mode: "idle", running: false, leased: false });
  const dialog = await openBrowser(page, session);
  await expect(dialog.getByRole("button", { name: "Take over" })).toBeVisible();
  await expect(dialog.getByText(/Chrome is stopped/)).toBeVisible();
  const bounds = await dialog.boundingBox();
  expect(bounds.x).toBeGreaterThanOrEqual(0);
  expect(bounds.x + bounds.width).toBeLessThanOrEqual(1440);
  expect(bounds.y).toBeGreaterThanOrEqual(0);
  expect(bounds.y + bounds.height).toBeLessThanOrEqual(900);
  expect(
    await page.evaluate(() => document.documentElement.scrollWidth),
  ).toBeLessThanOrEqual(1440);
});

test.describe("phone browser sheet", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
  });

  // The screen and the bar keep one geometry from loading to hand-back: no
  // state inserts anything into the flow, and control changes reuse the one
  // viewer connection.
  test("keeps one geometry and one connection through every state", async ({
    page,
    session,
  }) => {
    const { serveFramebuffer } = await import("./rfb-fixture.js");
    const remote = await serveFramebuffer(page);
    const status = await browserStatus(page, {
      mode: "agent",
      running: true,
      controller: false,
    });
    await page.goto(`/#session=${session.id}`);
    await remote.prepare();
    await page.getByRole("button", { name: "Open browser" }).click();
    const dialog = page.getByRole("dialog", { name: "Browser", exact: true });
    const screen = dialog.locator(".browser-screen");
    const bar = dialog.locator(".browser-bar");
    const primary = dialog.locator(".browser-primary");
    await expect(dialog.locator(".browser-rfb canvas")).toBeVisible();
    // Full screen on a phone.
    expect(await rect(dialog)).toEqual([0, 0, 390, 844]);
    const geometry = async () => [
      await rect(screen),
      await rect(bar),
      await rect(primary),
    ];
    const resting = await geometry();
    await expect(dialog.getByText("Agent", { exact: true })).toBeVisible();
    for (const name of ["Keyboard", "Copy", "Paste"])
      await expect(dialog.getByRole("button", { name })).toBeDisabled();

    Object.assign(status, { mode: "human", controller: true, leased: true });
    await expect(dialog.getByRole("button", { name: "Done" })).toBeVisible();
    await expect(dialog.getByRole("button", { name: "Paste" })).toBeEnabled();
    expect(await geometry()).toEqual(resting);

    // The keyboard's special keys replace the bar's contents, same height.
    await dialog.getByRole("button", { name: "Keyboard" }).click();
    await expect(dialog.getByLabel("Special keys")).toBeVisible();
    expect(await rect(bar)).toEqual(resting[1]);
    expect(await rect(screen)).toEqual(resting[0]);
    await dialog.getByRole("button", { name: "Hide keyboard" }).click();

    // Without clipboard access, Paste opens a card over the screen.
    await dialog.getByRole("button", { name: "Paste" }).click();
    const card = dialog.locator(".browser-card");
    await expect(card).toBeVisible();
    expect(await geometry()).toEqual(resting);
    // Real touches reach the card: the screen's gesture layer lets them by.
    await card.getByLabel(/Paste here/).tap();
    await expect(card.getByLabel(/Paste here/)).toBeFocused();
    await card.getByLabel(/Paste here/).fill("secret");
    await card.getByRole("button", { name: "Send" }).tap();
    await expect.poll(() => remote.clipboard.client).toBe("secret");
    await expect(card).toHaveCount(0);

    Object.assign(status, { mode: "agent", controller: false, leased: false });
    await expect(
      dialog.getByRole("button", { name: "Take over" }),
    ).toBeVisible();
    expect(await geometry()).toEqual(resting);
    expect(remote.connections.count).toBe(1);
    // A new display (Chrome restarted) is the one reason to reconnect.
    status.display = 2;
    await expect.poll(() => remote.connections.count).toBe(2);
    expect(
      await page.evaluate(() => document.documentElement.scrollWidth),
    ).toBeLessThanOrEqual(390);
  });

  test("direct touch clicks, scrolls, drags, right-clicks and zooms", async ({
    page,
    session,
  }) => {
    const { serveFramebuffer, touch } = await import("./rfb-fixture.js");
    const remote = await serveFramebuffer(page);
    const errors = [];
    page.on("pageerror", (error) => errors.push(error.message));
    await browserStatus(page, {
      mode: "human",
      running: true,
      controller: true,
      leased: true,
    });
    await page.goto(`/#session=${session.id}`);
    await remote.prepare();
    await page.getByRole("button", { name: "Open browser" }).click();
    const dialog = page.getByRole("dialog", { name: "Browser", exact: true });
    const viewport = dialog.getByLabel("Browser viewport");
    const canvas = dialog.locator(".browser-rfb canvas");
    await expect(dialog.getByRole("button", { name: "Paste" })).toBeEnabled();
    const box = await canvas.boundingBox();
    const at = (fx, fy) => ({
      x: box.x + box.width * fx,
      y: box.y + box.height * fy,
    });
    const masks = () => remote.pointers.splice(0).map((p) => p.buttons);

    // Tap: one click exactly where the finger landed.
    const tap = at(0.25, 0.5);
    await touch(viewport, "pointerdown", 1, tap.x, tap.y);
    await touch(viewport, "pointerup", 1, tap.x, tap.y);
    await expect.poll(() => remote.pointers.length).toBeGreaterThan(2);
    const last = remote.pointers.at(-1);
    expect(Math.abs(last.x - remote.width * 0.25)).toBeLessThanOrEqual(3);
    expect(Math.abs(last.y - remote.height * 0.5)).toBeLessThanOrEqual(3);
    expect(masks()).toEqual([0, 1, 0]);

    // Drag: wheel notches, content follows the finger (up = scroll down).
    const from = at(0.5, 0.8);
    await touch(viewport, "pointerdown", 2, from.x, from.y);
    await touch(viewport, "pointermove", 2, from.x, from.y - 60);
    await touch(viewport, "pointermove", 2, from.x, from.y - 120);
    await touch(viewport, "pointerup", 2, from.x, from.y - 120);
    await expect.poll(() => remote.pointers.length).toBeGreaterThan(0);
    const wheel = masks();
    expect(wheel).toContain(1 << 4);
    expect(wheel.every((mask) => mask === 0 || mask === 1 << 4)).toBe(true);

    // Hold, then drag: the left button stays down until the finger lifts.
    const hold = at(0.4, 0.4);
    await touch(viewport, "pointerdown", 3, hold.x, hold.y);
    await page.waitForTimeout(500);
    await touch(viewport, "pointermove", 3, hold.x + 40, hold.y);
    await touch(viewport, "pointerup", 3, hold.x + 40, hold.y);
    await expect.poll(() => remote.pointers.at(-1)?.buttons).toBe(0);
    const dragged = masks();
    expect(dragged.slice(0, -1).every((mask) => mask === 1)).toBe(true);
    expect(dragged.length).toBeGreaterThanOrEqual(3);

    // Two-finger tap: a right-click.
    const left = at(0.45, 0.5),
      right = at(0.55, 0.5);
    await touch(viewport, "pointerdown", 4, left.x, left.y);
    await touch(viewport, "pointerdown", 5, right.x, right.y);
    await touch(viewport, "pointerup", 4, left.x, left.y);
    await touch(viewport, "pointerup", 5, right.x, right.y);
    await expect.poll(() => masks()).toEqual([0, 4, 0]);

    // Pinch: zooms this device's view only; nothing reaches Chrome.
    const view = await viewport.boundingBox();
    const y = view.y + view.height / 2;
    await touch(viewport, "pointerdown", 6, view.x + view.width * 0.4, y);
    await touch(viewport, "pointerdown", 7, view.x + view.width * 0.6, y);
    await touch(viewport, "pointermove", 6, view.x + view.width * 0.2, y);
    await touch(viewport, "pointermove", 7, view.x + view.width * 0.8, y);
    await touch(viewport, "pointerup", 6, view.x + view.width * 0.2, y);
    await touch(viewport, "pointerup", 7, view.x + view.width * 0.8, y);
    await expect
      .poll(async () => (await canvas.boundingBox()).width / view.width)
      .toBeGreaterThan(2);
    expect(masks()).toEqual([]);
    // noVNC's own touch cursor stays hidden behind the modal.
    expect(
      await page.evaluate(() =>
        [...document.querySelectorAll("body > canvas")].every(
          (node) => getComputedStyle(node).display === "none",
        ),
      ),
    ).toBe(true);
    expect(errors).toEqual([]);
  });

  test("types live into Chrome with special keys and a sticky Ctrl", async ({
    page,
    session,
    browserName,
  }) => {
    test.skip(browserName !== "chromium", "needs CDP text insertion");
    const { serveFramebuffer } = await import("./rfb-fixture.js");
    const remote = await serveFramebuffer(page);
    await browserStatus(page, {
      mode: "human",
      running: true,
      controller: true,
      leased: true,
    });
    await page.goto(`/#session=${session.id}`);
    await remote.prepare();
    await page.getByRole("button", { name: "Open browser" }).click();
    const dialog = page.getByRole("dialog", { name: "Browser", exact: true });
    await dialog.getByRole("button", { name: "Keyboard" }).click();
    await expect(dialog.getByLabel("Type into Chrome")).toBeFocused();
    await expect(dialog.getByLabel("Browser viewport")).toBeInViewport();
    const pressed = () =>
      remote.keys.filter((key) => key.down).map((key) => key.keysym);
    await page.keyboard.insertText("Hé");
    await page.keyboard.press("Escape");
    await expect.poll(pressed).toEqual([0x48, 0xe9, 0xff1b]);
    // Special-key buttons keep the keyboard open; Ctrl applies once.
    await dialog.getByRole("button", { name: "Ctrl" }).click();
    await expect(dialog.getByLabel("Type into Chrome")).toBeFocused();
    await page.keyboard.insertText("a");
    await page.keyboard.insertText("b");
    await expect
      .poll(pressed)
      .toEqual([0x48, 0xe9, 0xff1b, 0xffe3, 0x61, 0x62]);
  });
});

test("watches an active agent without taking control", async ({
  page,
  session,
}) => {
  await browserStatus(page, {
    mode: "agent",
    running: true,
    leased: false,
    controller: false,
  });
  const dialog = await openBrowser(page, session);
  await expect(dialog.getByLabel("Browser viewport")).toHaveCount(1);
  await expect(dialog.getByText("Agent", { exact: true })).toBeVisible();
  await expect(dialog.getByRole("button", { name: "Take over" })).toBeVisible();
  for (const name of ["Copy", "Paste"])
    await expect(dialog.getByRole("button", { name })).toBeDisabled();
  // The app-wide zoom lock holds with the viewer open and after it closes.
  const meta = page.locator('meta[name="viewport"]');
  await expect(meta).toHaveAttribute("content", /user-scalable=no/);
  await page.keyboard.press("Escape");
  await expect(dialog).toHaveCount(0);
  await expect(meta).toHaveAttribute("content", /user-scalable=no/);
});

for (const [label, status, state, primary] of [
  [
    "watches a running browser nobody drives",
    { mode: "idle" },
    "Watching",
    "Take over",
  ],
  [
    "tells the driver when the agent waits for them",
    { mode: "human", controller: true, leased: true, waiting: true },
    "Your turn",
    "Done",
  ],
  [
    "asks the user to take over when the agent waits",
    { mode: "human", controller: false, leased: false, waiting: true },
    "Needs you",
    "Take over",
  ],
]) {
  test(label, async ({ page, session }) => {
    await browserStatus(page, { running: true, ...status });
    const dialog = await openBrowser(page, session);
    await expect(dialog.getByText(state, { exact: true })).toBeVisible();
    await expect(dialog.getByLabel("Browser viewport")).toHaveCount(1);
    await expect(dialog.getByRole("button", { name: primary })).toBeVisible();
  });
}

test("creates and selects a persistent Chrome profile", async ({
  page,
  session,
}) => {
  const status = await browserStatus(page, {
    mode: "idle",
    running: false,
    leased: false,
  });
  status.profiles = [{ id: "default", name: "Default" }];
  const actions = [];
  await page.route("**/api/command", (route) => {
    const command = route.request().postDataJSON();
    actions.push(command.action);
    if (command.action === "create_profile") {
      status.profiles.push({
        id: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        name: command.name.trim(),
      });
      return route.fulfill({
        json: {
          accepted: true,
          pending: false,
          result: { created_profile_id: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" },
        },
      });
    }
    if (command.action === "select_profile")
      status.profile_id = command.profile_id;
    return route.fulfill({
      json: { accepted: true, pending: false, result: { ok: true } },
    });
  });
  const dialog = await openBrowser(page, session);
  // Status, page and profiles share the one menu on the status pill.
  await dialog
    .getByRole("button", { name: /Browser status and profiles/ })
    .click();
  await dialog.getByRole("button", { name: "New profile" }).click();
  await dialog.getByLabel("New Chrome profile name").fill("Work");
  await dialog.getByRole("button", { name: "Create and use" }).click();
  await expect(
    dialog.getByLabel("Chrome profile", { exact: true }),
  ).toHaveValue("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  expect(actions).toEqual(["create_profile", "select_profile"]);
  // A named profile shows on the pill.
  await expect(dialog.locator(".browser-status")).toContainText("Work");
});

test("profile sign-in explicitly reopens Chrome and returns the same profile", async ({
  page,
  session,
}) => {
  const status = await browserStatus(page, {
    mode: "human",
    running: true,
    controller: true,
    leased: true,
    profile_setup: false,
  });
  const actions = [];
  await page.route("**/api/command", (route) => {
    const { action } = route.request().postDataJSON();
    actions.push(action);
    status.profile_setup = action === "setup_profile";
    if (action === "done")
      Object.assign(status, { mode: "idle", controller: false, leased: false });
    return route.fulfill({
      json: { accepted: true, pending: false, result: status },
    });
  });
  const dialog = await openBrowser(page, session);
  await dialog
    .getByRole("button", { name: /Browser status and profiles/ })
    .click();
  await dialog
    .getByRole("button", { name: "Sign in to profile", exact: true })
    .click();
  await expect(dialog.getByText(/Done reopens this profile/)).toBeVisible();
  await page.keyboard.press("Escape");
  await dialog.getByRole("button", { name: "Done", exact: true }).click();
  await expect(
    dialog.getByRole("button", { name: "Take over", exact: true }),
  ).toBeVisible();
  expect(status.profile_id).toBe("default");
  expect(actions).toEqual(["setup_profile", "done"]);
});

test("desktop Copy and Paste carry text between Chrome and the device", async ({
  page,
  context,
  session,
  browserName,
}) => {
  test.skip(browserName !== "chromium", "clipboard permissions");
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  const { serveFramebuffer } = await import("./rfb-fixture.js");
  const remote = await serveFramebuffer(page);
  await browserStatus(page, {
    mode: "human",
    running: true,
    controller: true,
    leased: true,
  });
  await page.goto(`/#session=${session.id}`);
  await remote.prepare();
  await page.getByRole("button", { name: "Open browser" }).click();
  const dialog = page.getByRole("dialog", { name: "Browser", exact: true });
  await expect(dialog.getByRole("button", { name: "Keyboard" })).toHaveCount(0);
  await expect(dialog.getByRole("button", { name: "Paste" })).toBeEnabled();

  await page.evaluate(() => navigator.clipboard.writeText("from device"));
  await dialog.getByRole("button", { name: "Paste" }).click();
  await expect.poll(() => remote.clipboard.client).toBe("from device");
  await expect
    .poll(() => remote.keys.some((key) => key.down && key.keysym === 0x76))
    .toBe(true);

  remote.clipboard.server = "from chrome";
  await dialog.getByRole("button", { name: "Copy" }).click();
  await expect(dialog.getByText("Copied")).toBeVisible();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(
    "from chrome",
  );
});
