// The terminal under the chat: shells of the user's on the host, in tabs.
// "Add to chat" puts a terminal's text into the composer and sends nothing.
import { test, expect, headerAction } from "./fixtures.js";

test("terminal tabs run commands and add their text to the chat", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await headerAction(page, "Terminal");
  const dock = page.getByRole("region", { name: "Terminal" });
  const shown = dock.locator(".terminal-tabpanel:not([hidden])");
  // Opening the panel starts a shell in the conversation's folder.
  await expect(dock.locator(".terminal-tab")).toHaveCount(1);
  await expect(shown.locator(".xterm-rows")).toContainText(/\S/);
  await shown.locator(".xterm").click();
  await page.keyboard.type("echo first-$((6*7))\n");
  await expect(shown.locator(".xterm-rows")).toContainText("first-42");

  await dock.getByRole("button", { name: "New terminal" }).click();
  await expect(dock.locator(".terminal-tab")).toHaveCount(2);
  await expect(shown.locator(".xterm-rows")).not.toContainText("first-42");
  await expect(shown.locator(".xterm-rows")).toContainText(/\S/);
  await shown.locator(".xterm").click();
  await page.keyboard.type("echo second-$((2*3))\n");
  await expect(shown.locator(".xterm-rows")).toContainText("second-6");

  // Back on the first tab, what it shows goes into the draft, unsent.
  await dock
    .locator(".terminal-tab")
    .first()
    .getByRole("button")
    .first()
    .click();
  await expect(shown.locator(".xterm-rows")).toContainText("first-42");
  await dock.getByRole("button", { name: "Add to chat" }).click();
  const composer = page.getByLabel("Message or guidance");
  await expect(composer).toHaveValue(
    /^```console\n[\s\S]*first-42[\s\S]*\n```$/,
  );
  await expect(composer).not.toHaveValue(/second-6/);
  await expect(composer).toBeFocused();

  // The panel hides and comes back with both shells still running.
  await headerAction(page, "Terminal");
  await expect(dock).toHaveCount(0);
  await headerAction(page, "Terminal");
  await expect(dock.locator(".terminal-tab")).toHaveCount(2);
});

test("on a phone the terminal takes the whole screen", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await headerAction(page, "Terminal");
  const sheet = page.getByRole("dialog", { name: "Terminal" });
  await expect(sheet.locator(".xterm-rows")).toContainText(/\S/);
  const box = await sheet.boundingBox();
  expect(box?.width).toBe(390);
  expect(box?.height).toBeGreaterThan(800);
  await expect(page.locator(".terminal-dock")).toHaveCount(0);
});

// iOS can keep reporting the keyboard's smaller visual viewport after the
// keyboard closes, until the page scrolls, and a terminal never scrolls the
// page. In the installed app the sheet returns to full height once nothing is
// being typed into.
test("on a phone the installed app's terminal regrows when the keyboard closes", async ({
  page,
  session,
}) => {
  await page.addInitScript(() => {
    const match = matchMedia.bind(window);
    window.matchMedia = (query) =>
      match(query === "(display-mode: standalone)" ? "all" : query);
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await headerAction(page, "Terminal");
  const sheet = page.getByRole("dialog", { name: "Terminal" });
  await expect(sheet.locator(".xterm-rows")).toContainText(/\S/);
  const input = sheet.locator(".xterm-helper-textarea");
  await input.focus();
  await page.evaluate(() => {
    Object.defineProperty(visualViewport, "height", {
      configurable: true,
      value: 420,
    });
    visualViewport.dispatchEvent(new Event("resize"));
  });
  await expect
    .poll(async () => Math.round((await sheet.boundingBox()).height))
    .toBeLessThan(440);
  // Done on the keyboard: focus leaves, the reported viewport stays small.
  await input.blur();
  await expect
    .poll(async () => Math.round((await sheet.boundingBox()).height))
    .toBeGreaterThan(800);
});
