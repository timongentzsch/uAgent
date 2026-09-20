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
