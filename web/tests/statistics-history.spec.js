// Statistics of a row the person paged back to: the dialog reads the
// conversation as the page holds it, and leaves the loaded pages in place.
import { test, expect, saveHistory } from "./fixtures.js";

test("an older row's statistics open without dropping the loaded pages", async ({
  page,
  host,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  // Eighty turns as the host saves them: a message of yours, its answer and
  // the turn's footer, whose menu holds the answer's statistics.
  const ids = [];
  const facts = {};
  for (let turn = 0; turn < 80; turn++) {
    const ask = 3 * turn + 1;
    const footer = ask + 2;
    ids.push(ask, ask + 1);
    facts[`m-${ask}`] = { turn_root: `m-${ask}` };
    facts[`m-${ask + 1}`] = {
      turn_root: `m-${ask}`,
      time: "2026-10-28T16:00:00.000Z",
    };
    facts[`m-${footer}`] = {
      kind: "turn_summary",
      id: `m-${footer}`,
      sequence: footer,
      turn_root: `m-${ask}`,
      time: "2026-10-28T16:00:01.000Z",
      summary: {
        turn: turn + 1,
        outcome: "complete",
        steps: 1,
        tool_calls: 0,
        duration_ms: 1000,
        usage: { input: 10, output: 5, cost: 0 },
      },
    };
  }
  await saveHistory(host, "long.json", {
    id: "long",
    title: "Long",
    turns: 80,
    messages: Array.from({ length: 160 }, (_, index) => ({
      role: index % 2 ? "assistant" : "user",
      content: `Message ${index}.`,
    })),
    display: { ids, facts, next: 241 },
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
  await page
    .locator('.turn-footer[data-message-id="m-3"]')
    .getByRole("button", { name: "Actions for this answer" })
    .click();
  await page
    .getByRole("menuitem", { name: "Model call statistics", exact: true })
    .click();
  const dialog = page.getByRole("dialog", { name: "Message statistics" });
  await expect(dialog).toContainText("Timestamp");
  await expect(dialog).not.toContainText("outside the loaded history");
  await page.keyboard.press("Escape");
  await expect(first).toHaveCount(1);
});
