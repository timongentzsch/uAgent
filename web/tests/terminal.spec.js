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
  await expect(dock.locator(".terminal-place")).toHaveText(/^Terminal 1 · /);
  await expect(dock.locator(".terminal-tab")).toHaveCount(0);
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
  await dock.getByRole("button", { name: "Terminal 1", exact: true }).click();
  await expect(shown.locator(".xterm-rows")).toContainText("first-42");
  await dock.getByRole("button", { name: "Terminal menu" }).click();
  await page.getByRole("menuitem", { name: "Add to chat" }).click();
  const composer = page.getByLabel("Message or guidance");
  await expect(composer).toHaveValue(
    /^```console\n[\s\S]*first-42[\s\S]*\n```$/,
  );
  await expect(composer).not.toHaveValue(/second-6/);
  await expect(composer).toBeFocused();

  // The page's own shells end with the panel; it opens again with a new one.
  await headerAction(page, "Terminal");
  await expect(dock).toHaveCount(0);
  await headerAction(page, "Terminal");
  await expect(dock.locator(".terminal-place")).toHaveText(/^Terminal 1 · /);
  await expect(shown.locator(".xterm-rows")).not.toContainText("first-42");
});

test("a terminal keeps its number, and a new one takes the lowest free", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await headerAction(page, "Terminal");
  const dock = page.getByRole("region", { name: "Terminal" });
  const place = dock.locator(".terminal-place");
  await expect(place).toHaveText(/^Terminal 1 · /);
  await dock.getByRole("button", { name: "New terminal" }).click();
  await expect(dock.locator(".terminal-tab")).toHaveCount(2);
  await expect(place).toHaveText(/^Terminal 2 · /);
  await dock.getByRole("button", { name: "Close Terminal 1" }).click();
  await expect(dock.locator(".terminal-tab")).toHaveCount(0);
  await expect(place).toHaveText(/^Terminal 2 · /);
  await dock.getByRole("button", { name: "New terminal" }).click();
  await expect(dock.locator(".terminal-tab")).toHaveCount(2);
  await expect(place).toHaveText(/^Terminal 1 · /);
});

test("Add to chat is in the menu only with a conversation to add to", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/");
  await headerAction(page, "Terminal");
  const dock = page.getByRole("region", { name: "Terminal" });
  await expect(dock.locator(".terminal-place")).toHaveText("Terminal 1 · ~");
  await dock.getByRole("button", { name: "Terminal menu" }).click();
  await expect(
    page.getByRole("menuitem", { name: "Close terminal" }),
  ).toBeVisible();
  await expect(page.getByRole("menuitem", { name: "Add to chat" })).toHaveCount(
    0,
  );
});

test("a folder's terminal opens in the page, keeps running and closes", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const place = page.locator(".place-head").first();
  await place.getByRole("button", { name: /^New in / }).click();
  await page.getByRole("menuitem", { name: "Terminal" }).click();
  const shell = page.getByRole("region", { name: /^Terminal in / });
  await expect(shell.locator(".xterm-rows")).toContainText(/\S/);
  await shell.locator(".xterm").click();
  await page.keyboard.type("echo kept-$((5*5))\n");
  await expect(shell.locator(".xterm-rows")).toContainText("kept-25");
  // Listed under its folder: away and back, it shows what it printed.
  await page
    .locator(".session", { hasText: session.title || "" })
    .first()
    .click();
  await expect(shell).toHaveCount(0);
  const row = page.locator(".place-body .session", { hasText: "Terminal" });
  await row.click();
  await expect(shell.locator(".xterm-rows")).toContainText("kept-25");
  await expect(row).toHaveAttribute("aria-current", "page");
  // Closing it ends the shell and shows the folder's overview.
  await shell.getByRole("button", { name: "Close terminal" }).click();
  await expect(
    page.getByRole("region", { name: /^Overview of / }),
  ).toBeVisible();
  await expect(row).toHaveCount(0);
});

test("a folder's terminal has a name and a menu to rename, pin and close it", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const nav = page.getByRole("navigation", { name: "Conversations" });
  const pinned = nav.getByRole("group", { name: "Pinned" });
  await page
    .locator(".place-head")
    .first()
    .getByRole("button", { name: /^New in / })
    .click();
  await page.getByRole("menuitem", { name: "Terminal" }).click();
  const shell = page.getByRole("region", { name: /^Terminal in / });
  await expect(shell.locator(".xterm-rows")).toContainText(/\S/);
  const row = nav.locator(".place-body .session-row", {
    hasText: "Terminal 1",
  });
  await expect(row.locator(".session-time")).toHaveCount(1);

  // The name is the person's to change.
  await row
    .getByRole("button", { name: "Terminal menu for Terminal 1" })
    .click();
  await page.getByRole("menuitem", { name: "Rename" }).click();
  await page.getByLabel("Terminal name").fill("Build");
  await page.getByRole("button", { name: "Save name" }).click();
  const renamed = nav.locator(".session-row", { hasText: "Build" });
  await expect(renamed).toHaveCount(1);

  // Pinned, it leaves its folder for the top; closing it removes the pin.
  await renamed
    .getByRole("button", { name: "Terminal menu for Build" })
    .click();
  await page.getByRole("menuitem", { name: "Pin", exact: true }).click();
  await expect(
    pinned.locator(".session-row", { hasText: "Build" }),
  ).toHaveCount(1);
  await expect(
    nav.locator(".place-body .session-row", { hasText: "Build" }),
  ).toHaveCount(0);
  await pinned.getByRole("button", { name: "Terminal menu for Build" }).click();
  await page.getByRole("menuitem", { name: "Close", exact: true }).click();
  await expect(
    page.getByRole("region", { name: /^Overview of / }),
  ).toBeVisible();
  await expect(nav.locator(".session-row", { hasText: "Build" })).toHaveCount(
    0,
  );
  await expect(pinned).toHaveCount(0);
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

// The same leftover in a browser tab: the keyboard's height reported with no
// keyboard is stale too.
test("on a phone a tab's terminal regrows when the keyboard closes", async ({
  page,
  session,
}) => {
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
  await input.blur();
  await expect
    .poll(async () => Math.round((await sheet.boundingBox()).height))
    .toBeGreaterThan(800);
});

test.describe("on a touch phone", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
  });

  test("the terminal fills the sheet between its bar and its keys", async ({
    page,
  }) => {
    await page.goto("/");
    await headerAction(page, "Terminal");
    const sheet = page.getByRole("dialog", { name: "Terminal" });
    await expect(sheet.locator(".xterm-rows")).toContainText(/\S/);
    await expect(sheet.locator(".terminal-place")).toHaveText("Terminal 1 · ~");
    await expect(sheet.locator(".terminal-tab")).toHaveCount(0);
    // Every control in the bar has a name.
    for (const button of await sheet.locator(".terminal-bar button").all())
      expect(await button.getAttribute("aria-label")).toBeTruthy();
    const box = async (selector) => sheet.locator(selector).boundingBox();
    const keys = await box(".terminal-keys");
    const view = await box(".terminal-view");
    const screen = await box(".xterm-screen");
    const frame = await sheet.boundingBox();
    const safeBottom = await page.evaluate(
      () =>
        parseFloat(
          getComputedStyle(document.getElementById("app")).paddingBottom,
        ) || 0,
    );
    expect(frame.y + frame.height - (keys.y + keys.height)).toBeLessThanOrEqual(
      safeBottom + 1,
    );
    expect(Math.abs(view.y + view.height - keys.y)).toBeLessThanOrEqual(1);
    expect(screen.y + screen.height).toBeLessThanOrEqual(keys.y);
    expect(screen.x + screen.width).toBeLessThanOrEqual(view.x + view.width);
    // What a row leaves over depends on the size, so a fit that counts its
    // padding as room overflows at only some: a full row's worth of heights,
    // and the common phone widths.
    const sizes = [
      ...Array.from({ length: 12 }, (_, step) => [390, 844 - step * 2]),
      ...[375, 393, 402, 412, 430].map((width) => [width, 844]),
    ];
    for (const [width, height] of sizes) {
      await page.setViewportSize({ width, height });
      await expect
        .poll(async () => (await box(".terminal-view")).width)
        .toBeCloseTo(width, 0);
      await page.evaluate(
        () =>
          new Promise((done) =>
            requestAnimationFrame(() => requestAnimationFrame(done)),
          ),
      );
      const room = await box(".terminal-view");
      const shown = await box(".xterm-screen");
      expect(shown.y + shown.height, `${width}x${height}`).toBeLessThanOrEqual(
        room.y + room.height,
      );
      expect(shown.x + shown.width, `${width}x${height}`).toBeLessThanOrEqual(
        room.x + room.width,
      );
    }
    await page.setViewportSize({ width: 390, height: 844 });
    // The keys share the width without clipping.
    for (const key of await sheet.locator(".key-bar > button").all()) {
      const rect = await key.boundingBox();
      expect(rect.x).toBeGreaterThanOrEqual(0);
      expect(rect.x + rect.width).toBeLessThanOrEqual(390);
    }
  });

  test("the keyboard key shows and hides the keyboard", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    await headerAction(page, "Terminal");
    const sheet = page.getByRole("dialog", { name: "Terminal" });
    await expect(sheet.locator(".xterm-rows")).toContainText(/\S/);
    const input = sheet.locator(".xterm-helper-textarea");
    // A touch screen does not focus the terminal on its own.
    await expect(input).not.toBeFocused();
    await sheet.getByRole("button", { name: "Show the keyboard" }).tap();
    await expect(input).toBeFocused();
    await sheet.getByRole("button", { name: "Hide the keyboard" }).tap();
    await expect(input).not.toBeFocused();
    await expect(
      sheet.getByRole("button", { name: "Show the keyboard" }),
    ).toBeVisible();
  });
});
