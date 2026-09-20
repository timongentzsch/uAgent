// Transcript must not statically import lazy dialog chunks: holding every
// dialog chunk must not block message or turn-footer rendering.
import { test, expect } from "./fixtures.js";

test("transcript renders while dialog chunks are held", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  const gates = {};
  for (const chunk of [
    "sidebar",
    "chat",
    "composer",
    "model-picker",
    "settings",
    "statistics",
    "raw",
    "conversation-actions",
    "prompt",
    "library",
    "scheduled",
    "decision",
  ]) {
    let release;
    const gate = new Promise((resolve) => (release = resolve));
    gates[chunk] = release;
    await page.route(`**/${chunk}-*.js`, async (route) => {
      await gate;
      await route.continue();
    });
  }
  const release = async (key) => {
    gates[key]();
    await page.waitForTimeout(200);
  };

  await page.goto(`/#session=${session.id}`);
  await release("sidebar");
  await release("chat");
  await release("composer");
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
  // Dialog chunks (statistics included) are still held: the tool rows
  // and the turn footer button must render anyway.
  await expect(page.locator(".transcript .tool-disclosure")).toHaveCount(2, {
    timeout: 60000,
  });
  await expect(
    page.getByRole("button", { name: "Turn statistics" }).first(),
  ).toBeVisible({ timeout: 30000 });
  // Its styles ship with the transcript (message.css), not the lazy
  // statistics chunk: already laid out before the dialog ever opens.
  await expect(
    page.getByRole("button", { name: "Turn statistics" }).first(),
  ).toHaveCSS("display", "flex");

  // Releasing the statistics chunk must light up the dialog on demand.
  await release("statistics");
  await page.getByRole("button", { name: "Turn statistics" }).first().click();
  await expect(page.locator("dialog dl.stats").first()).toBeVisible({
    timeout: 15000,
  });
});
