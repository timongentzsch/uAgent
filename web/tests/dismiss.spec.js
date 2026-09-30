import { test, expect } from "./fixtures.js";

// The system back gesture closes the topmost layer, as in a native app, and
// layers closed from the interface leave no entry for back to reopen.
test.describe("back closes what is open", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
  });

  const layerDepth = (page) => page.evaluate(() => history.state?.layer ?? 0);

  test("dialogs, drawer and pages close on back, one layer at a time", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    await expect(page.getByLabel("Message or guidance")).toBeVisible();

    // A dialog: back closes it and stays in the conversation.
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    const settings = page.getByRole("dialog", { name: "Settings" });
    await expect(settings).toBeVisible();
    expect(await layerDepth(page)).toBe(1);
    await page.goBack();
    await expect(settings).toHaveCount(0);
    await expect(page).toHaveURL(new RegExp(session.id));
    expect(await layerDepth(page)).toBe(0);

    // Closing from the interface removes its entry: back does not reopen it
    // and does not leave the conversation either.
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    await settings.getByRole("button", { name: "Close settings" }).click();
    await expect(settings).toHaveCount(0);
    await expect.poll(() => layerDepth(page)).toBe(0);
    // Forward never reopens a closed layer.
    await page.goForward();
    await expect(settings).toHaveCount(0);

    // The drawer, then a page opened from it: back unwinds page by page.
    await page
      .getByRole("button", { name: "Open sessions", exact: true })
      .tap();
    const drawer = page.getByRole("dialog", { name: "Sessions" });
    await drawer.getByRole("button", { name: "Library", exact: true }).tap();
    await expect(drawer).toHaveCount(0);
    await expect(
      page.getByRole("heading", { name: "Library", exact: true }),
    ).toBeVisible();
    await page.goBack();
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    await expect(page).toHaveURL(new RegExp(session.id));
  });

  test("settings unwinds section, then list, then closes", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    const settings = page.getByRole("dialog", { name: "Settings" });
    await settings
      .locator(".settings-nav")
      .getByRole("button", { name: "Permissions", exact: true })
      .tap();
    await expect(
      settings.getByRole("heading", { name: "Permissions", exact: true }),
    ).toBeVisible();
    await page.goBack();
    await expect(
      settings.getByRole("heading", { name: "Settings", exact: true }),
    ).toBeVisible();
    await expect(settings.locator(".settings-nav")).toBeVisible();
    await page.goBack();
    await expect(settings).toHaveCount(0);
    await expect(page).toHaveURL(new RegExp(session.id));
  });

  test("a dialog swapped in from a drilled section leaves no stale entry", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    const before = await page.evaluate(() => history.length);
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    const settings = page.getByRole("dialog", { name: "Settings" });
    await settings
      .locator(".settings-nav")
      .getByRole("button", { name: "Agent", exact: true })
      .tap();
    await settings.getByRole("button", { name: /Instructions/ }).tap();
    const prompt = page.getByRole("dialog", { name: "Instructions" });
    await expect(prompt).toBeVisible();
    // One back closes the swapped-in dialog and reaches the conversation.
    await page.goBack();
    await expect(prompt).toHaveCount(0);
    await expect.poll(() => layerDepth(page)).toBe(0);
    // Closing from the interface leaves history where it started.
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    await settings.getByRole("button", { name: "Close settings" }).click();
    await expect.poll(() => layerDepth(page)).toBe(0);
    expect(await page.evaluate(() => history.length)).toBeGreaterThanOrEqual(
      before,
    );
  });

  test("a reload on a layer entry opens nothing and back still works", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    await expect(page.getByRole("dialog", { name: "Settings" })).toBeVisible();
    await page.reload();
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    await expect(page.getByRole("dialog", { name: "Settings" })).toHaveCount(0);
    expect(await layerDepth(page)).toBe(0);
  });
});

test("dialogs play their exit before they leave", async ({ page, session }) => {
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  const settings = page.getByRole("dialog", { name: "Settings" });
  await expect(settings).toBeVisible();
  // Record the leave state as it happens: the exit takes 200ms.
  await page.evaluate(() => {
    const dialog = document.querySelector("dialog[open]");
    new MutationObserver(() => {
      if (dialog.hasAttribute("data-leaving")) window.__left = true;
    }).observe(dialog, { attributes: true });
  });
  await page.keyboard.press("Escape");
  await expect(settings).toHaveCount(0);
  expect(await page.evaluate(() => window.__left)).toBe(true);
});

// A menu item that opens a dialog must leave its menu able to reopen: WebKit
// drops the popover's closing toggle event when a modal dialog opens.
test("a menu reopens after the dialog it opened closes", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  const head = page.locator(".conversation-head");
  const menu = head.getByRole("button", { name: "Conversation menu" });
  const rename = head.getByRole("menuitem", { name: "Rename", exact: true });
  await menu.click();
  await rename.click();
  const dialog = page.getByRole("dialog", { name: "Rename conversation" });
  await dialog.getByRole("button", { name: /^Close/ }).click();
  await expect(dialog).toHaveCount(0);
  await menu.click();
  await expect(rename).toBeVisible();
});

// The phone drawer slides in from the left: a closed dialog must not be
// drawn, or it would skip its starting style and appear in place.
test.describe("phone drawer", () => {
  test.use({
    reducedMotion: "no-preference",
    viewport: { width: 390, height: 800 },
  });
  test("slides in from the left", async ({ page }) => {
    await page.goto("/");
    await expect(page.getByLabel("Open sessions")).toBeVisible();
    const left = await page.evaluate(async () => {
      document.querySelector('[aria-label="Open sessions"]').click();
      await new Promise((resolve) => requestAnimationFrame(resolve));
      await new Promise((resolve) => requestAnimationFrame(resolve));
      return document.querySelector("dialog.drawer").getBoundingClientRect()
        .left;
    });
    expect(left).toBeLessThan(0);
    await expect
      .poll(() =>
        page.evaluate(
          () =>
            document.querySelector("dialog.drawer").getBoundingClientRect()
              .left,
        ),
      )
      .toBe(0);
  });
});
