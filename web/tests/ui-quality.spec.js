import { test, expect } from "./fixtures.js";
import { readFile } from "node:fs/promises";

const geometry = (locator) =>
  locator.evaluate((node) => {
    const { x, y, width, height } = node.getBoundingClientRect();
    return { x, y, width, height };
  });

test("browser shell keeps its bounds through cold code and data loading", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const manifest = JSON.parse(
    await readFile(new URL("../dist/.vite/manifest.json", import.meta.url)),
  );
  let releaseCode, releaseData;
  const code = new Promise((resolve) => (releaseCode = resolve));
  const data = new Promise((resolve) => (releaseData = resolve));
  await page.route(
    `**/${manifest["src/features/browser/browser.tsx"].file}`,
    async (route) => {
      await code;
      await route.continue();
    },
  );
  let statusReads = 0;
  await page.route("**/api/browser/status", async (route) => {
    if (++statusReads > 1) await data;
    await route.fulfill({
      json: {
        ok: true,
        mode: "idle",
        running: false,
        profiles: [{ id: "default", name: "Default" }],
      },
    });
  });
  try {
    await page.goto(`/#session=${session.id}`);
    const trigger = page.getByRole("button", { name: "Open browser" });
    await trigger.focus();
    await trigger.press("Enter");
    const dialog = page.getByRole("dialog", { name: "Browser", exact: true });
    await expect(dialog.getByRole("status")).toHaveText("Loading browser…");
    const shell = await geometry(dialog);
    releaseCode();
    await expect
      .poll(() => page.locator('link[href*="browser-"]').count())
      .toBeGreaterThan(0);
    expect(await geometry(dialog)).toEqual(shell);
    releaseData();
    await expect(
      dialog.getByRole("button", { name: "Take control" }),
    ).toBeVisible();
    expect(await geometry(dialog)).toEqual(shell);
    await page.keyboard.press("Escape");
    await expect(dialog).toHaveCount(0);
    await expect(trigger).toBeFocused();
  } finally {
    releaseCode();
    releaseData();
  }
});

test("loading showcase keeps a fixed shell and keyboard focus", async ({
  page,
}) => {
  await page.goto("/ui.html");
  const trigger = page.getByRole("button", { name: "Open loading dialog" });
  await trigger.focus();
  await trigger.press("Enter");
  const dialog = page.getByRole("dialog", {
    name: "Loading example",
    exact: true,
  });
  await expect(dialog.getByRole("status")).toBeVisible();
  const shell = await geometry(dialog);
  await expect(dialog.getByLabel("Loaded value")).toBeVisible();
  expect(await geometry(dialog)).toEqual(shell);
  await page.keyboard.press("Escape");
  await expect(trigger).toBeFocused();
});

test.describe("touch interaction", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
  });
  test("touch does not acquire desktop hover and small-scale fields keep a readable font", async ({
    page,
  }) => {
    await page.goto("/ui.html");
    expect(
      await page.evaluate(() => matchMedia("(hover: hover)").matches),
    ).toBe(false);
    const button = page.getByRole("button", {
      name: "Secondary action",
      exact: true,
    });
    const color = await button.evaluate(
      (node) => getComputedStyle(node).backgroundColor,
    );
    await button.tap();
    await expect(button).toHaveCSS("background-color", color);
    await page.getByLabel("Zoom", { exact: true }).fill("50");
    for (const field of [
      page.getByRole("textbox", { name: /^Text / }),
      page.getByRole("searchbox", { name: "Search", exact: true }),
      page.getByRole("textbox", { name: "Long text", exact: true }),
    ]) {
      expect(
        await field.evaluate((node) =>
          parseFloat(getComputedStyle(node).fontSize),
        ),
      ).toBeGreaterThanOrEqual(16);
    }
  });
});
