import { test, expect } from "./fixtures.js";

for (const viewport of [
  { width: 1440, height: 900 },
  { width: 390, height: 844 },
]) {
  test(`browser panel fits the viewport at ${viewport.width}px`, async ({
    page,
    session,
  }) => {
    await page.setViewportSize(viewport);
    await page.route("**/api/browser/status", (route) =>
      route.fulfill({
        json: {
          ok: true,
          mode: "idle",
          running: false,
          leased: false,
          profile_id: "default",
          profiles: [{ id: "default", name: "Default" }],
        },
      }),
    );
    await page.goto(`/#session=${session.id}`);
    await page.getByRole("button", { name: "Open browser" }).click();
    const dialog = page.getByRole("dialog", { name: "Browser" });
    await expect(
      dialog.getByRole("button", { name: "Take control" }),
    ).toBeVisible();
    const bounds = await dialog.boundingBox();
    expect(bounds.x).toBeGreaterThanOrEqual(0);
    expect(bounds.x + bounds.width).toBeLessThanOrEqual(viewport.width);
    expect(bounds.y).toBeGreaterThanOrEqual(0);
    expect(bounds.y + bounds.height).toBeLessThanOrEqual(viewport.height);
    expect(
      await page.evaluate(() => document.documentElement.scrollWidth),
    ).toBeLessThanOrEqual(viewport.width);
  });
}

test.describe("phone browser viewer", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
  });

  test("keeps touch and text controls inside the dialog", async ({
    page,
    session,
  }) => {
    await page.route("**/api/browser/status", (route) =>
      route.fulfill({
        json: {
          ok: true,
          mode: "human",
          running: true,
          leased: true,
          controller: true,
          generation: 1,
          profile_id: "default",
          profiles: [{ id: "default", name: "Default" }],
        },
      }),
    );
    await page.goto(`/#session=${session.id}`);
    await page.getByRole("button", { name: "Open browser" }).click();
    const dialog = page.getByRole("dialog", { name: "Browser" });
    await expect(dialog.getByLabel("Browser trackpad")).toBeVisible();
    await expect(dialog.getByLabel("Browser viewport")).toBeVisible();
    // Touch devices always get the trackpad; there is nothing to toggle.
    await expect(dialog.getByRole("button", { name: "Trackpad" })).toHaveCount(
      0,
    );
    await expect(dialog.getByRole("button", { name: "Left" })).toBeVisible();
    await expect(dialog.getByRole("button", { name: "Right" })).toBeVisible();
    for (const name of ["Keyboard", "Copy", "Paste"])
      await expect(dialog.getByRole("button", { name })).toBeVisible();
    await expect(
      dialog.getByRole("button", { name: "Hand back" }),
    ).toBeVisible();
    await page.setViewportSize({ width: 390, height: 600 });
    await expect(page.locator("html")).toHaveCSS("--viewport-height", "600px");
    const done = await dialog
      .getByRole("button", { name: "Hand back" })
      .boundingBox();
    expect(done.y + done.height).toBeLessThanOrEqual(600);
    await expect(
      dialog.getByRole("button", { name: "Actual size" }),
    ).toHaveCount(0);
    const bounds = await dialog.boundingBox();
    expect(bounds.x).toBeGreaterThanOrEqual(0);
    expect(bounds.x + bounds.width).toBeLessThanOrEqual(390);
    expect(bounds.y).toBeGreaterThanOrEqual(0);
    expect(bounds.y + bounds.height).toBeLessThanOrEqual(600);
    expect(
      await page.evaluate(() => document.documentElement.scrollWidth),
    ).toBeLessThanOrEqual(390);
  });
});

test("watches an active agent without taking control", async ({
  page,
  session,
}) => {
  await page.route("**/api/browser/status", (route) =>
    route.fulfill({
      json: {
        ok: true,
        mode: "agent",
        running: true,
        leased: false,
        controller: false,
        generation: 3,
        profile_id: "default",
        profiles: [{ id: "default", name: "Default" }],
      },
    }),
  );
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Open browser" }).click();
  const dialog = page.getByRole("dialog", { name: "Browser" });
  await expect(dialog.getByLabel("Read-only browser display")).toBeVisible();
  await expect(dialog.getByLabel("Browser viewport")).toHaveCount(1);
  await expect(dialog.getByText(/Agent working/)).toBeVisible();
  await expect(
    dialog.getByRole("button", { name: "Take control" }),
  ).toBeVisible();
  for (const name of ["Keyboard", "Copy", "Paste"])
    await expect(dialog.getByRole("button", { name })).toHaveCount(0);
  await expect(dialog.getByLabel("Browser trackpad")).toHaveCount(0);
  // Pinch belongs to the remote display only while the viewer is open.
  const viewport = page.locator('meta[name="viewport"]');
  await expect(viewport).toHaveAttribute("content", /user-scalable=no/);
  await page.keyboard.press("Escape");
  await expect(dialog).toHaveCount(0);
  await expect(viewport).not.toHaveAttribute("content", /user-scalable=no/);
});

for (const [label, status, driver] of [
  ["watches a running browser nobody drives", { mode: "idle" }, /Watching/],
  [
    "tells the driver when the agent waits for the browser",
    { mode: "human", controller: true, leased: true, waiting: true },
    /Agent is waiting/,
  ],
]) {
  test(label, async ({ page, session }) => {
    await page.route("**/api/browser/status", (route) =>
      route.fulfill({
        json: {
          ok: true,
          running: true,
          generation: 4,
          profile_id: "default",
          profiles: [{ id: "default", name: "Default" }],
          ...status,
        },
      }),
    );
    await page.goto(`/#session=${session.id}`);
    await page.getByRole("button", { name: "Open browser" }).click();
    const dialog = page.getByRole("dialog", { name: "Browser" });
    await expect(dialog.getByText(driver)).toBeVisible();
    await expect(dialog.getByLabel("Browser viewport")).toHaveCount(1);
    await expect(
      dialog.getByRole("button", {
        name: status.controller ? "Hand back" : "Take control",
      }),
    ).toBeVisible();
  });
}

test("creates and selects a persistent Chrome profile", async ({
  page,
  session,
}) => {
  const status = {
    ok: true,
    mode: "idle",
    running: false,
    leased: false,
    profile_id: "default",
    profiles: [{ id: "default", name: "Default" }],
  };
  const actions = [];
  await page.route("**/api/browser/status", (route) =>
    route.fulfill({ json: status }),
  );
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
          result: {
            created_profile_id: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          },
        },
      });
    }
    if (command.action === "select_profile")
      status.profile_id = command.profile_id;
    return route.fulfill({
      json: { accepted: true, pending: false, result: { ok: true } },
    });
  });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Open browser" }).click();
  const dialog = page.getByRole("dialog", { name: "Browser" });
  // Profile actions are rare; they live in one menu above the screen.
  await dialog.getByRole("button", { name: "Profiles" }).click();
  await dialog.getByRole("button", { name: "New profile" }).click();
  await dialog.getByLabel("New Chrome profile name").fill("Work");
  await dialog.getByRole("button", { name: "Create and use" }).click();
  await expect(
    dialog.getByLabel("Chrome profile", { exact: true }),
  ).toHaveValue("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  expect(actions).toEqual(["create_profile", "select_profile"]);
});

test("profile sign-in explicitly reopens Chrome and returns the same profile", async ({
  page,
  session,
}) => {
  const status = {
    ok: true,
    mode: "human",
    running: true,
    controller: true,
    leased: true,
    generation: 1,
    profile_id: "default",
    profiles: [{ id: "default", name: "Default" }],
    profile_setup: false,
  };
  const actions = [];
  await page.route("**/api/browser/status", (route) =>
    route.fulfill({ json: status }),
  );
  await page.route("**/api/command", (route) => {
    const { action } = route.request().postDataJSON();
    actions.push(action);
    status.profile_setup = action === "setup_profile";
    status.generation++;
    if (action === "done") {
      status.mode = "idle";
      status.controller = false;
      status.leased = false;
    }
    return route.fulfill({
      json: { accepted: true, pending: false, result: status },
    });
  });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Open browser" }).click();
  const dialog = page.getByRole("dialog", { name: "Browser", exact: true });
  // Sign-in sits in the profile menu, beside New profile.
  await dialog.getByRole("button", { name: "Profiles" }).click();
  await expect(
    dialog
      .locator(".browser-profile-controls")
      .getByRole("button", { name: "Sign in to profile", exact: true }),
  ).toBeVisible();
  await dialog
    .getByRole("button", { name: "Sign in to profile", exact: true })
    .click();
  await expect(
    dialog.getByText(/Hand back reopens this profile/),
  ).toBeVisible();
  await expect(
    dialog.getByRole("button", { name: "Sign in to profile", exact: true }),
  ).toHaveCount(0);
  await dialog.getByRole("button", { name: "Hand back", exact: true }).click();
  await expect(
    dialog.getByRole("button", { name: "Take control", exact: true }),
  ).toBeVisible();
  expect(status.profile_id).toBe("default");
  expect(actions).toEqual(["setup_profile", "done"]);
});

test.describe("real noVNC input in a mobile modal", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
  });
  test("paints the pointer above the dialog and follows it at zoom", async ({
    page,
    session,
  }, testInfo) => {
    const { serveFramebuffer, touch, cursor } =
      await import("./rfb-fixture.js");
    const remote = await serveFramebuffer(page);
    const errors = [];
    page.on("pageerror", (error) => errors.push(error.message));
    await page.route("**/api/browser/status", (route) =>
      route.fulfill({
        json: {
          ok: true,
          mode: "human",
          running: true,
          controller: true,
          generation: 1,
        },
      }),
    );
    await page.goto(`/#session=${session.id}`);
    await remote.prepare();
    await page.getByRole("button", { name: "Open browser" }).click();
    const dialog = page.getByRole("dialog", { name: "Browser", exact: true });
    await expect(dialog.getByText("Connected", { exact: true })).toBeVisible();
    const marker = dialog.locator(".browser-pointer");
    await expect(marker).toBeVisible();
    expect(
      await marker.evaluate((node) => !!node.closest("dialog:modal")),
    ).toBe(true);
    // The pointer is the server's own cursor image, not a local stand-in.
    await expect
      .poll(() => marker.evaluate((node) => [node.width, node.height]))
      .toEqual([cursor.width, cursor.height]);
    const hotspot = async () => {
      const box = await marker.boundingBox();
      const [x, y] = (await marker.getAttribute("data-hotspot"))
        .split(",")
        .map(Number);
      return { x: box.x + x, y: box.y + y };
    };
    const viewport = dialog.getByLabel("Browser viewport");
    const canvas = dialog.locator(".browser-rfb canvas");
    const pad = dialog.getByLabel("Browser trackpad");
    const move = async (dx, dy) => {
      const box = await pad.boundingBox();
      const x = box.x + box.width / 2,
        y = box.y + box.height / 2;
      await touch(pad, "pointerdown", 1, x, y);
      await touch(pad, "pointermove", 1, x + dx, y + dy);
      await touch(pad, "pointerup", 1, x + dx, y + dy);
    };
    const assertPosition = async () => {
      const point = await hotspot(),
        display = await canvas.boundingBox();
      const expected = {
        x: Math.min(
          remote.width - 1,
          Math.round(((point.x - display.x) / display.width) * remote.width),
        ),
        y: Math.min(
          remote.height - 1,
          Math.round(((point.y - display.y) / display.height) * remote.height),
        ),
      };
      // WebKit can quantize MouseEvent coordinates to CSS pixels; RFB then
      // quantizes again to framebuffer pixels. Account for the display scale.
      const pixelQuantization =
        Math.ceil(
          Math.max(
            remote.width / display.width,
            remote.height / display.height,
          ),
        ) + 1;
      await expect
        .poll(() => {
          const actual = remote.pointers.at(-1);
          return actual
            ? Math.max(
                Math.abs(actual.x - expected.x),
                Math.abs(actual.y - expected.y),
              )
            : Infinity;
        })
        .toBeLessThanOrEqual(pixelQuantization);
    };
    await move(12, 8);
    await assertPosition();
    const view = await viewport.boundingBox();
    await touch(
      viewport,
      "pointerdown",
      2,
      view.x + view.width * 0.4,
      view.y + view.height / 2,
    );
    await touch(
      viewport,
      "pointerdown",
      3,
      view.x + view.width * 0.6,
      view.y + view.height / 2,
    );
    await touch(
      viewport,
      "pointermove",
      2,
      view.x + view.width * 0.2,
      view.y + view.height / 2,
    );
    await touch(
      viewport,
      "pointermove",
      3,
      view.x + view.width * 0.8,
      view.y + view.height / 2,
    );
    await touch(
      viewport,
      "pointerup",
      2,
      view.x + view.width * 0.2,
      view.y + view.height / 2,
    );
    await touch(
      viewport,
      "pointerup",
      3,
      view.x + view.width * 0.8,
      view.y + view.height / 2,
    );
    await expect
      .poll(async () => (await canvas.boundingBox()).width / view.width)
      .toBeGreaterThan(2);
    const target = dialog.locator(".browser-rfb");
    const before = await target.evaluate((node) => node.style.transform);
    await move(200, 100);
    await assertPosition();
    expect(await target.evaluate((node) => node.style.transform)).not.toBe(
      before,
    );
    const point = await hotspot();
    expect(point.x).toBeGreaterThanOrEqual(view.x);
    expect(point.x).toBeLessThan(view.x + view.width);
    expect(point.y).toBeGreaterThanOrEqual(view.y);
    expect(point.y).toBeLessThan(view.y + view.height);
    // Like a native pointer, the image past the hotspot may run under the
    // screen's edge; the screen clips it.
    expect(
      await marker.evaluate(
        (node) =>
          getComputedStyle(node.closest(".browser-screen")).overflow ===
          "hidden",
      ),
    ).toBe(true);
    const right = dialog.getByRole("button", { name: "Right", exact: true });
    const button = await right.boundingBox();
    await touch(right, "pointerdown", 4, button.x + 5, button.y + 5);
    await touch(right, "pointerup", 4, button.x + 5, button.y + 5);
    expect(remote.pointers.some((point) => point.buttons === 4)).toBe(true);
    await dialog.screenshot({
      path: testInfo.outputPath("visible-vnc-cursor.png"),
    });
    expect(errors).toEqual([]);
  });
});

const controlledBrowser = (page) =>
  page.route("**/api/browser/status", (route) =>
    route.fulfill({
      json: {
        ok: true,
        mode: "human",
        running: true,
        controller: true,
        generation: 1,
      },
    }),
  );

test.describe("phone browser keyboard", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
  });

  test("types live into Chrome with special keys and a sticky Ctrl", async ({
    page,
    session,
    browserName,
  }) => {
    test.skip(browserName !== "chromium", "needs CDP text insertion");
    const { serveFramebuffer, cursor } = await import("./rfb-fixture.js");
    const remote = await serveFramebuffer(page);
    await controlledBrowser(page);
    await page.goto(`/#session=${session.id}`);
    await remote.prepare();
    await page.getByRole("button", { name: "Open browser" }).click();
    const dialog = page.getByRole("dialog", { name: "Browser", exact: true });
    await expect(dialog.getByText("Connected", { exact: true })).toBeVisible();
    // The server's pointer shrinks with the remote screen, like Chrome's own.
    const pointer = dialog.locator(".browser-pointer");
    await expect
      .poll(() => pointer.evaluate((node) => node.width))
      .toBe(cursor.width);
    const drawn = (await pointer.boundingBox()).width;
    expect(drawn).toBeLessThan(cursor.width);
    expect(drawn).toBeGreaterThanOrEqual(cursor.width * 0.45 - 0.5);
    await dialog.getByRole("button", { name: "Keyboard" }).click();
    await expect(dialog.getByLabel("Type into Chrome")).toBeFocused();
    // Typing keeps the screen in view: the trackpad steps aside.
    await expect(dialog.getByLabel("Browser trackpad")).toBeHidden();
    await expect(dialog.getByLabel("Browser viewport")).toBeInViewport();
    await expect(dialog.getByRole("button", { name: "Keys" })).toHaveCount(0);
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
  await controlledBrowser(page);
  await page.goto(`/#session=${session.id}`);
  await remote.prepare();
  await page.getByRole("button", { name: "Open browser" }).click();
  const dialog = page.getByRole("dialog", { name: "Browser", exact: true });
  await expect(dialog.getByText("Connected", { exact: true })).toBeVisible();
  await expect(dialog.getByLabel("Browser trackpad")).toHaveCount(0);
  await expect(dialog.getByRole("button", { name: "Keyboard" })).toHaveCount(0);

  await page.evaluate(() => navigator.clipboard.writeText("from device"));
  await dialog.getByRole("button", { name: "Paste" }).click();
  await expect.poll(() => remote.clipboard.client).toBe("from device");
  await expect
    .poll(() => remote.keys.some((key) => key.down && key.keysym === 0x76))
    .toBe(true);

  remote.clipboard.server = "from chrome";
  await dialog.getByRole("button", { name: "Copy" }).click();
  await expect(dialog.getByText("Copied to this device.")).toBeVisible();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(
    "from chrome",
  );
});
