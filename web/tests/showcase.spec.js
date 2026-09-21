import { test, expect } from "./fixtures.js";

test("static UI showcase renders shared flat controls and scales them", async ({
  page,
}, testInfo) => {
  await page.setViewportSize({ width: 1100, height: 900 });
  await page.addInitScript(() => localStorage.setItem("uagent-theme", "dark"));
  await page.goto("/ui.html");

  await expect(
    page.getByRole("heading", { name: "µAgent UI showcase", exact: true }),
  ).toBeVisible();
  const secondary = page.getByRole("button", {
    name: "Secondary action",
    exact: true,
  });
  const select = page.getByRole("combobox").nth(1);
  const styles = await Promise.all(
    [secondary, select].map((control) =>
      control.evaluate((element) => {
        const style = getComputedStyle(element);
        return {
          background: style.backgroundColor,
          border: style.borderTopColor,
          radius: style.borderRadius,
          shadow: style.boxShadow,
        };
      }),
    ),
  );
  expect(styles[0].background).toBe(styles[1].background);
  expect(styles[0].border).toBe("rgba(0, 0, 0, 0)");
  expect(styles[1].border).toBe("rgba(0, 0, 0, 0)");
  expect(styles[0].radius).toBe("0px");
  expect(styles[0].shadow).toBe("none");

  const normalHeight = await secondary.evaluate(
    (element) => element.getBoundingClientRect().height,
  );
  const normalWidth = await page
    .locator(".showcase")
    .evaluate((element) => element.getBoundingClientRect().width);
  await page.getByLabel("Zoom", { exact: true }).fill("50");
  await expect(page.locator("html")).toHaveCSS("--zoom", "0.5");
  const smallHeight = await secondary.evaluate(
    (element) => element.getBoundingClientRect().height,
  );
  expect(smallHeight).toBeLessThan(normalHeight * 0.7);
  const compactWidth = await page
    .locator(".showcase")
    .evaluate((element) => element.getBoundingClientRect().width);
  expect(compactWidth).toBeGreaterThan(normalWidth);

  await page.getByRole("button", { name: "Example menu" }).click();
  await expect(page.getByRole("menu", { name: "Example menu" })).toBeVisible();
  await page.keyboard.press("Escape");
  await page.getByRole("button", { name: "Open dialog" }).click();
  const dialog = page.getByRole("dialog", { name: "Example dialog" });
  await expect(dialog).toBeVisible();
  await dialog.getByRole("button", { name: "Confirm" }).click();
  await expect(dialog).toHaveCount(0);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  await page.screenshot({
    path: testInfo.outputPath("ui-showcase.png"),
    fullPage: true,
  });
});
