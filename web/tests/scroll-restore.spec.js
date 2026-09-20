// Session return restores the saved position unfollowed: switching away
// and back must neither pin to the bottom nor strand the reader. Seeded
// histories keep this deterministic (no streaming or backend timing).
import { test, expect } from "./fixtures.js";
import { mkdir, writeFile } from "node:fs/promises";

async function seed(home, project, file, id, title) {
  const messages = Array.from({ length: 60 }, (_, index) => ({
    role: index % 2 ? "assistant" : "user",
    content: `Seeded ${id} message ${index}. ` + "Filler text here. ".repeat(8),
  }));
  await writeFile(
    `${home}/.uagent/history/${file}`,
    JSON.stringify({
      format: 3,
      cwd: project,
      model: "test",
      session_id: id,
      title,
      turns: 30,
    }) +
      "\n" +
      JSON.stringify({
        messages,
        message_kinds: messages.map((item) => item.role),
        archive: [],
        archive_dropped_segments: 0,
        context_tokens: 0,
        usage: {},
        tool_displays: {},
      }),
    { mode: 0o600 },
  );
}

test("returning to a session restores its position unfollowed", async ({
  page,
  host: fixture,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await mkdir(`${fixture.home}/.uagent/history`, { recursive: true });
  await seed(fixture.home, fixture.project, "a.json", "restore-a", "Restore A");
  await seed(fixture.home, fixture.project, "b.json", "restore-b", "Restore B");
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await page.getByRole("button", { name: /Restore A/ }).click();
  const box = page.locator(".transcript");
  await expect
    .poll(() => box.evaluate((el) => el.scrollHeight - el.clientHeight), {
      timeout: 15000,
    })
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
    { timeout: 15000 },
  );
  await page.getByRole("button", { name: /Restore A/ }).click();
  await expect(page.locator(".conversation-head h1")).toContainText(
    "Restore A",
    { timeout: 15000 },
  );
  // Unfollowed (Jump visible, not pinned) and back at the saved spot.
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeVisible({ timeout: 15000 });
  await expect
    .poll(() => box.evaluate((element) => element.scrollTop), {
      timeout: 15000,
    })
    .toBeLessThan(124);
});
