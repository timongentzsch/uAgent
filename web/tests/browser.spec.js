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
    await expect(
      dialog.getByRole("button", { name: "Trackpad" }),
    ).toHaveAttribute("aria-pressed", "true");
    await expect(dialog.getByRole("button", { name: "Left" })).toBeVisible();
    await expect(dialog.getByRole("button", { name: "Right" })).toBeVisible();
    await dialog.getByRole("button", { name: "Trackpad" }).click();
    await expect(dialog.getByLabel("Browser trackpad")).toHaveCount(0);
    await dialog.getByRole("button", { name: "Trackpad" }).click();
    await dialog.getByRole("button", { name: "Text & keys" }).click();
    await expect(dialog.getByLabel("Text for Chrome")).toBeVisible();
    await expect(dialog.getByLabel("Browser trackpad")).toBeVisible();
    await expect(dialog.getByRole("button", { name: "Done" })).toBeVisible();
    await page.setViewportSize({ width: 390, height: 600 });
    await expect(page.locator("html")).toHaveCSS("--viewport-height", "600px");
    const done = await dialog
      .getByRole("button", { name: "Done" })
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
  await expect(dialog.getByText(/Watching agent/)).toBeVisible();
  await expect(
    dialog.getByRole("button", { name: "Take control" }),
  ).toBeVisible();
  await expect(dialog.getByRole("button", { name: "Text & keys" })).toHaveCount(
    0,
  );
  await expect(dialog.getByRole("button", { name: "Trackpad" })).toHaveCount(0);
});

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
  await dialog.getByRole("button", { name: "New profile" }).click();
  await dialog.getByLabel("New Chrome profile name").fill("Work");
  await dialog.getByRole("button", { name: "Create and use" }).click();
  await expect(
    dialog.getByLabel("Chrome profile", { exact: true }),
  ).toHaveValue("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  expect(actions).toEqual(["create_profile", "select_profile"]);
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
    const { serveFramebuffer, touch } = await import("./rfb-fixture.js");
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
      const cursor = await marker.boundingBox(),
        display = await canvas.boundingBox();
      const expected = {
        x: Math.min(
          remote.width - 1,
          Math.round(((cursor.x - display.x) / display.width) * remote.width),
        ),
        y: Math.min(
          remote.height - 1,
          Math.round(((cursor.y - display.y) / display.height) * remote.height),
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
    const point = await marker.boundingBox();
    expect(point.x).toBeGreaterThanOrEqual(view.x);
    expect(point.x).toBeLessThan(view.x + view.width);
    expect(point.y).toBeGreaterThanOrEqual(view.y);
    expect(point.y).toBeLessThan(view.y + view.height);
    const arrow = await marker.locator("path").boundingBox();
    expect(arrow.x).toBeGreaterThanOrEqual(view.x);
    expect(arrow.x + arrow.width).toBeLessThanOrEqual(view.x + view.width);
    expect(arrow.y).toBeGreaterThanOrEqual(view.y);
    expect(arrow.y + arrow.height).toBeLessThanOrEqual(view.y + view.height);
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
