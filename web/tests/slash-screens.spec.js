// A slash command with a screen opens it when typed bare, as a click would.
import { test, expect, online } from "./fixtures.js";

test("bare slash commands open their screens", async ({ page, session }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  const composer = page.getByLabel("Message or guidance");
  await online(page);
  const run = async (text) => {
    await composer.fill(text);
    await composer.press("Enter");
  };

  // Each lands on its row of Settings, and the address names it.
  const settings = page.getByRole("dialog", { name: "Settings" });
  const topic = (name) =>
    settings
      .getByRole("navigation", { name: "Settings topics" })
      .getByRole("button", { name: new RegExp(`^${name}`) });
  for (const [command, name, row, address] of [
    ["/tools", "This conversation", "tools", "conversation/tools"],
    ["/detail", "This conversation", "view", "conversation/view"],
    ["/permissions", "Approvals and safety", "rules", "approvals/rules"],
    ["/mcp", "Tools and extensions", "mcp-servers", "tools/mcp-servers"],
  ]) {
    await run(command);
    await expect(topic(name)).toHaveAttribute("aria-current", "page");
    await expect(settings.locator(`[data-row="${row}"]`)).toBeInViewport();
    await expect(page).toHaveURL(new RegExp(`settings=${address}$`));
    await page.keyboard.press("Escape");
    await expect(settings).toHaveCount(0);
  }
  await run("/tools");
  await expect(settings.getByText(/serialized schema/)).toBeVisible();
  await page.keyboard.press("Escape");
  await run("/config");
  await expect(topic("This conversation")).toHaveAttribute(
    "aria-current",
    "page",
  );
  await page.keyboard.press("Escape");
  await expect(settings).toHaveCount(0);

  const conversation = page.getByRole("button", { name: /\(no messages\)/ });
  for (const [command, tab] of [
    ["/skills", "Skills"],
    ["/memory", "Memories"],
  ]) {
    await run(command);
    await expect(
      page
        .getByRole("navigation", { name: "Library filters" })
        .getByRole("button", { name: new RegExp(`^${tab}`) }),
    ).toHaveAttribute("aria-current", "true");
    await conversation.click();
    await expect(composer).toBeEnabled();
  }

  await run("/clear");
  await expect(
    page.getByText("/clear works only in a terminal."),
  ).toBeVisible();

  // On a wide screen the list is already beside the conversation: it
  // searches instead.
  await run("/sessions");
  await expect(
    page
      .getByRole("dialog", { name: "Search" })
      .getByRole("combobox", { name: /^Search conversations/ }),
  ).toBeFocused();
});
