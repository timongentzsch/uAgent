// One row per tool call, live and retained. Live frames, retained
// assistant-tools entries and retained result blocks must collapse into
// a single row — never a stuck "Running" call plus an orphan result —
// both during the turn and after a reload replays retained history.
import { test, expect } from "./fixtures.js";

test("tool calls render exactly once and never stick on Running", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(180000);
  await page.goto(`/#session=${session.id}`);
  const prompt = page.getByLabel("Message or guidance");
  await expect(prompt).toBeVisible();

  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
  await command("permissions", {
    session_id: session.id,
    generation: session.generation,
    mode: "yolo",
  });
  await prompt.fill("Exploration probe");
  await prompt.press("Enter");
  const rows = page.locator(".transcript .tool-disclosure");
  await expect(rows).toHaveCount(2, { timeout: 90000 });
  await expect(
    page.getByRole("button", { name: "Turn statistics" }).first(),
  ).toBeVisible({ timeout: 60000 });
  // Turn is over: no row may still read Running, and the count holds.
  await expect(rows).toHaveCount(2);
  await expect(
    page.locator(".transcript .tool-disclosure", { hasText: "Running" }),
  ).toHaveCount(0, { timeout: 30000 });

  // Reload replays retained blocks through the same pipeline: still one
  // row per call, still nothing stuck.
  await page.reload();
  await expect(page.getByLabel("Message or guidance")).toBeVisible();
  await expect(page.locator(".transcript .tool-disclosure")).toHaveCount(2, {
    timeout: 60000,
  });
  await expect(
    page.locator(".transcript .tool-disclosure", { hasText: "Running" }),
  ).toHaveCount(0, { timeout: 30000 });
});
