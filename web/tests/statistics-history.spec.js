// Statistics of a row the person paged back to: the dialog reads the
// conversation as the page holds it, and leaves the loaded pages in place.
import { test, expect, saveHistory } from "./fixtures.js";

test("an older row's statistics open without dropping the loaded pages", async ({
  page,
  host,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await saveHistory(host, "long.json", {
    id: "long",
    title: "Long",
    turns: 80,
    messages: Array.from({ length: 160 }, (_, index) => ({
      role: index % 2 ? "assistant" : "user",
      content: `Message ${index}.`,
    })),
  });
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await page.getByRole("button", { name: /Long/ }).click();
  const first = page
    .locator(".transcript .message")
    .filter({ hasText: "Message 1." });
  const older = page.getByRole("button", {
    name: "Load older retained messages",
    exact: true,
  });
  // Page back until the row is there; the button hides while a page loads.
  await expect(async () => {
    if (!(await first.count())) await older.click({ timeout: 2000 });
    await expect(first).toHaveCount(1, { timeout: 1000 });
  }).toPass({ timeout: 30000 });
  await first.getByLabel(/^Actions for /).click();
  await page.getByRole("menuitem", { name: "Statistics", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "Message statistics" });
  await expect(dialog).toContainText("Timestamp");
  await expect(dialog).not.toContainText("outside the loaded history");
  await page.keyboard.press("Escape");
  await expect(first).toHaveCount(1);
});
