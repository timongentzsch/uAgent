import {
  connected,
  test,
  expect,
  leaveEnd,
  openMore,
  savedConversation,
  startInFolder,
} from "./fixtures.js";

test.describe("mobile navigation and commands", () => {
  test.use({
    isMobile: true,
    hasTouch: true,
    viewport: { width: 390, height: 600 },
  });
  test("conversation navigation preserves the document and drafts; slash commands complete without sending", async ({
    page,
    context,
    host: fixture,
  }, testInfo) => {
    await page.setViewportSize({ width: 1280, height: 900 });
    await page.goto("/");
    await expect(connected(page)).toBeVisible();

    const firstTitle = `Navigation A ${testInfo.project.name}`;
    const secondTitle = `Navigation B ${testInfo.project.name}`;
    // Both have a turn already: the list leaves out an empty one that is not
    // open.
    const { id: first } = await savedConversation(
        fixture,
        page.request,
        firstTitle,
      ),
      { id: second } = await savedConversation(
        fixture,
        page.request,
        secondTitle,
      );
    let documents = 0,
      streams = 0;
    page.on("request", (request) => {
      if (request.resourceType() === "document") documents++;
      if (new URL(request.url()).pathname === "/api/events") streams++;
    });
    await page.evaluate(() => {
      window.navigationProof = "same-document";
    });
    await page.setViewportSize({ width: 390, height: 600 });
    const choose = async (title) => {
      await page
        .getByRole("button", { name: "Open conversations", exact: true })
        .click();
      await page
        .getByRole("dialog", { name: "Conversations" })
        .getByRole("button", { name: new RegExp(title) })
        .click();
    };
    await choose(firstTitle);
    const prompt = page.getByLabel("Message or guidance");
    await expect(prompt).toBeVisible();
    await expect(page.locator(".composer .activity-button")).toHaveCount(0);
    const send = page.getByRole("button", { name: "Send", exact: true });
    await prompt.fill("/model mock/model-b");
    await prompt.press("Enter");
    await expect(prompt).toHaveValue("/model mock/model-b\n");
    await prompt.fill("/model mock/model-b");
    await send.tap();
    await expect(
      page.getByRole("button", { name: "Model and effort", exact: true }),
    ).toContainText("model-b");
    await prompt.fill("Navigation message");
    await send.tap();
    await expect(
      page.getByRole("heading", { name: "Verified response" }),
    ).toBeVisible();
    await expect(page.locator(".composer .activity-button")).toHaveCount(0);
    await expect(
      page.getByText("[image: blocked]", { exact: true }),
    ).toBeVisible();
    if (testInfo.project.name === "chromium")
      await context.grantPermissions(["clipboard-read", "clipboard-write"]);
    const code = page.locator(".code-block");
    const copy = code.getByRole("button", { name: "Copy code", exact: true });
    await expect(code.locator(".code-copy")).toHaveCSS("opacity", "1");
    const copyBox = await copy.boundingBox();
    expect(Math.min(copyBox.width, copyBox.height)).toBeGreaterThanOrEqual(44);
    await copy.tap();
    await expect(
      code.getByRole("button", { name: "Copied!", exact: true }),
    ).toBeVisible();
    await leaveEnd(page.locator(".transcript"));
    await expect(
      page.getByRole("button", { name: "Jump to latest" }),
    ).toBeVisible();
    await prompt.fill("Draft A");
    await choose(secondTitle);
    await expect(page).toHaveURL(new RegExp(second));
    await expect(
      page.getByRole("heading", { name: secondTitle, exact: true }),
    ).toBeVisible();
    await expect(prompt).toBeVisible();
    await prompt.fill("Draft B");
    const retiredTranscript = await page.locator(".transcript").elementHandle();
    await choose(firstTitle);
    await expect(page).toHaveURL(new RegExp(first));
    await expect(
      page.getByRole("heading", { name: firstTitle, exact: true }),
    ).toBeVisible();
    await expect(prompt).toHaveValue("Draft A");
    // A queued scroll from the previous surface cannot rewrite this session.
    await retiredTranscript.evaluate((element) => {
      element.scrollTop = 0;
      element.dispatchEvent(new Event("scroll"));
    });
    await retiredTranscript.dispose();
    await expect
      .poll(() =>
        page
          .locator(".transcript")
          .evaluate((element) => Math.abs(element.scrollTop - 24)),
      )
      .toBeLessThan(2);
    expect(await page.evaluate(() => history.scrollRestoration)).toBe("manual");
    await page.goBack();
    await expect(page).toHaveURL(new RegExp(second));
    await expect(prompt).toHaveValue("Draft B");
    // Back landed on the closed drawer's entry, which the app steps past on
    // the next frame; a forward before that collides with the step.
    await expect
      .poll(() => page.evaluate(() => history.state?.layer ?? 0))
      .toBe(0);
    await page.goForward();
    await expect(page).toHaveURL(new RegExp(first));
    await expect(prompt).toHaveValue("Draft A");
    await expect
      .poll(() =>
        page
          .locator(".transcript")
          .evaluate((element) => Math.abs(element.scrollTop - 24)),
      )
      .toBeLessThan(2);
    expect(await page.evaluate(() => window.navigationProof)).toBe(
      "same-document",
    );
    expect(documents).toBe(0);
    expect(streams).toBe(0);
    await prompt.fill("/comp");
    await prompt.press("Tab");
    await expect(prompt).toHaveValue("/compact");
    await expect(page.locator(".message.user")).toHaveCount(1);
    await prompt.fill("/");
    const offered = page.getByRole("option");
    await expect(offered.filter({ hasText: "/compact" })).toHaveCount(1);
    for (const owned of ["/attach", "/model", "/yolo", "/tell"])
      await expect(offered.filter({ hasText: owned })).toHaveCount(0);
    await prompt.press("ArrowDown");
    await expect(prompt).toHaveAttribute("aria-activedescendant", "command-0");
    await prompt.press("Enter");
    await expect(prompt).toHaveValue("/compact");
    await prompt.fill("/sta");
    await page.getByRole("option", { name: /\/status/ }).tap();
    await expect(prompt).toHaveValue("/status");
    await expect(prompt).toBeFocused();
    await send.tap();
    await expect(
      page.getByRole("dialog", { name: "Full content" }),
    ).toContainText('"topic": "status"');
    await page.keyboard.press("Escape");
    await prompt.fill("/context");
    await send.tap();
    await expect(
      page.getByRole("dialog", { name: "Raw context", exact: true }),
    ).toBeVisible();
    await page.keyboard.press("Escape");
    await prompt.fill("/help");
    await send.tap();
    await expect(
      page.getByRole("dialog", { name: "Full content" }),
    ).toContainText("/permissions");
    await page.keyboard.press("Escape");
    await prompt.fill("/http 1 response");
    await send.tap();
    await expect(
      page
        .getByRole("dialog")
        .getByRole("tab", { name: "Response", exact: true }),
    ).toHaveAttribute("aria-selected", "true");
    await expect(page.getByRole("dialog")).toContainText("Verified response");
    await page.keyboard.press("Escape");
    const chooser = page.waitForEvent("filechooser");
    await prompt.fill("/attach");
    await send.tap();
    await (await chooser).setFiles([]);
    await prompt.fill("/fork Slash fork");
    await send.tap();
    await expect(page.locator(".conversation-head h1")).toHaveText(
      "Slash fork",
    );
    await expect(page.locator(".composer .activity-button")).toHaveCount(0);
    await prompt.fill("/sessions");
    await send.tap();
    await expect(
      page.getByRole("dialog", { name: "Conversations", exact: true }),
    ).toBeVisible();
    await page
      .getByRole("button", { name: "Close conversations", exact: true })
      .tap();
    await page.getByRole("button", { name: "Open conversations" }).click();
    await startInFolder(page, fixture.project);
    await expect(page.locator(".message.user")).toHaveCount(0);
    await prompt.fill("Survives reload");
    // Saved after the next paint; reload once it is stored.
    await expect
      .poll(() => page.evaluate(() => localStorage.getItem("uagent-drafts")))
      .toContain("Survives reload");
    await page.reload();
    await expect(prompt).toHaveValue("Survives reload");
    await page
      .locator(".conversation-head")
      .getByLabel("Conversation menu", { exact: true })
      .tap();
    await page.getByRole("menuitem", { name: "More", exact: true }).tap();
    await page.getByRole("menuitem", { name: "Close conversation" }).tap();
    await expect(prompt).toBeVisible();
    await prompt.fill("/att");
  });
});

// An iPhone home-screen app gives the left edge to the conversations drawer:
// moving between conversations adds no entry Safari could swipe back to.
test.describe("in an iPhone home-screen app", () => {
  test.use({
    isMobile: true,
    hasTouch: true,
    viewport: { width: 390, height: 700 },
  });
  test("conversations replace their history entry", async ({ page, host }) => {
    await page.addInitScript(() => {
      Object.defineProperty(navigator, "standalone", { value: true });
      window.pushed = [];
      const push = history.pushState.bind(history);
      history.pushState = (state, title, url) => {
        window.pushed.push(String(url ?? ""));
        push(state, title, url);
      };
    });
    const titles = ["Edge A", "Edge B"];
    // Where in history the app is, where the browser can tell.
    const index = () =>
      page.evaluate(() => globalThis.navigation?.currentEntry?.index ?? 0);
    for (const title of titles)
      await savedConversation(host, page.request, title);
    await page.goto("/");
    await expect(
      page.getByRole("button", { name: "Open conversations", exact: true }),
    ).toBeVisible();
    const start = await index();
    for (const title of titles) {
      await page
        .getByRole("button", { name: "Open conversations", exact: true })
        .click();
      await page
        .getByRole("dialog", { name: "Conversations" })
        .getByRole("button", { name: new RegExp(title) })
        .click();
      await expect(
        page.getByRole("heading", { name: title, exact: true }),
      ).toBeVisible();
      // The drawer's own entry is gone once it has closed.
      await expect
        .poll(() => page.evaluate(() => history.state?.layer ?? 0))
        .toBe(0);
    }
    await expect.poll(index).toBe(start);
    // The drawer adds its own entry while open; a conversation never does.
    expect(
      await page.evaluate(() =>
        window.pushed.filter((url) => url.includes("session=")),
      ),
    ).toEqual([]);
  });
});
