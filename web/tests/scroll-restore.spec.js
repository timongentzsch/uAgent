// Session return restores the saved position unfollowed: switching away
// and back must neither pin to the bottom nor strand the reader. Seeded
// histories keep this deterministic (no streaming or backend timing).
import { test, expect, saveHistory } from "./fixtures.js";

function seed(host, file, id, title) {
  const messages = Array.from({ length: 60 }, (_, index) => ({
    role: index % 2 ? "assistant" : "user",
    content: `Seeded ${id} message ${index}. ` + "Filler text here. ".repeat(8),
  }));
  return saveHistory(host, file, { id, title, turns: 30, messages });
}

test("returning to a session restores its position unfollowed", async ({
  page,
  host: fixture,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await seed(fixture, "a.json", "restore-a", "Restore A");
  await seed(fixture, "b.json", "restore-b", "Restore B");
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await page.getByRole("button", { name: /Restore A/ }).click();
  const box = page.locator(".transcript");
  await expect
    .poll(() => box.evaluate((el) => el.scrollHeight - el.clientHeight))
    .toBeGreaterThan(500);
  await box.evaluate((element) => {
    element.scrollTop = 24;
  });
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeVisible();
  await page.getByRole("button", { name: /Restore B/ }).click();
  await expect(page.locator(".conversation-head h1")).toContainText(
    "Restore B",
  );
  await page.getByRole("button", { name: /Restore A/ }).click();
  await expect(page.locator(".conversation-head h1")).toContainText(
    "Restore A",
  );
  // Unfollowed (Jump visible, not pinned) and back at the saved spot.
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeVisible();
  await expect
    .poll(() => box.evaluate((element) => element.scrollTop))
    .toBeLessThan(124);
});
