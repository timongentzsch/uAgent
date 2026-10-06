// A slash command with a screen opens it when typed bare, as a click would.
import { test, expect } from "./fixtures.js";

test("bare slash commands open their screens", async ({ page, session }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  const composer = page.getByLabel("Message or guidance");
  await expect(composer).toBeEnabled();
  const run = async (text) => {
    await composer.fill(text);
    await composer.press("Enter");
  };

  await run("/tools");
  const tools = page.getByRole("dialog", { name: "Tools", exact: true });
  await expect(tools).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(tools).toHaveCount(0);

  await run("/permissions");
  const settings = page.getByRole("dialog", { name: "Settings" });
  await expect(
    settings.getByRole("button", { name: "All conversations" }),
  ).toHaveAttribute("aria-current", "page");
  await page.keyboard.press("Escape");
  await expect(settings).toHaveCount(0);

  // The Library replaces the conversation; its row brings it back.
  const conversation = page.getByRole("button", { name: /\(no messages\)/ });
  for (const [command, tab] of [
    ["/skills", "Skills"],
    ["/memory", "Memories"],
  ]) {
    await run(command);
    await expect(
      page.getByRole("button", { name: tab, exact: true }),
    ).toHaveAttribute("aria-pressed", "true");
    await conversation.click();
    await expect(composer).toBeEnabled();
  }

  // On a wide screen the list is already beside the conversation.
  await run("/sessions");
  await expect(page.getByLabel("Find a conversation")).toBeFocused();
});
