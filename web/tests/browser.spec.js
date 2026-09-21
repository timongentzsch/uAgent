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
        json: { ok: true, mode: "idle", running: false, leased: false },
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
        },
      }),
    );
    await page.goto(`/#session=${session.id}`);
    await page.getByRole("button", { name: "Open browser" }).click();
    const dialog = page.getByRole("dialog", { name: "Browser" });
    await expect(dialog.getByLabel("Browser trackpad")).toBeVisible();
    await dialog.getByRole("button", { name: "Text & keys" }).click();
    await expect(dialog.getByLabel("Text for Chrome")).toBeVisible();
    await expect(dialog.getByLabel("Browser trackpad")).toBeHidden();
    await expect(dialog.getByRole("button", { name: "Done" })).toBeVisible();
    await page.setViewportSize({ width: 390, height: 600 });
    const done = await dialog
      .getByRole("button", { name: "Done" })
      .boundingBox();
    expect(done.y + done.height).toBeLessThanOrEqual(600);
    await dialog.getByRole("button", { name: "Actual size" }).click();
    await expect(
      dialog.getByRole("button", { name: "Fit screen" }),
    ).toBeVisible();
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
