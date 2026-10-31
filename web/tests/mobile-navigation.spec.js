import { test, expect, leaveEnd, newFolder } from "./fixtures.js";

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
    await expect(page.getByText("Connected", { exact: true })).toBeVisible();

    const call = async (kind, session, extra = {}) => {
      const response = await page.request.post("/api/command", {
        headers: { Origin: fixture.origin },
        data: {
          v: 2,
          request_id: crypto.randomUUID().replaceAll("-", ""),
          kind,
          ...(session
            ? { session_id: session.id, generation: session.generation || "" }
            : {}),
          ...extra,
        },
      });
      const result = await response.json();
      expect(response.ok(), JSON.stringify(result)).toBe(true);
      return result;
    };
    const make = async (title) => {
      const { session } = await call("create", null, { cwd: fixture.project });
      await call("rename", session, { title });
      await call("activate", session);
      return session.id;
    };
    const firstTitle = `Navigation A ${testInfo.project.name}`;
    const secondTitle = `Navigation B ${testInfo.project.name}`;
    const first = await make(firstTitle),
      second = await make(secondTitle);
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
      await page.getByRole("button", { name: new RegExp(title) }).click();
    };
    await choose(firstTitle);
    const prompt = page.getByLabel("Message or guidance");
    await expect(prompt).toBeVisible();
    await expect(page.locator(".composer .activity-button")).toHaveCount(0);
    // On a soft keyboard return writes a new line; Send sends.
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
    // The list offers what the page has no control for: nothing for files,
    // the model or the approval mode, which have their own.
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
    await newFolder(page, fixture.project);
    await page
      .getByRole("dialog", { name: "New folder" })
      .getByRole("button", { name: "Start conversation" })
      .click();
    await expect(page.locator(".composer .activity-button")).toHaveCount(0);
    await expect(page.locator(".message.user")).toHaveCount(0);
    // An unsent draft survives a reload (an evicted installed app).
    await prompt.fill("Survives reload");
    // Saved after the next paint; reload once it is stored.
    await expect
      .poll(() => page.evaluate(() => localStorage.getItem("uagent-drafts")))
      .toContain("Survives reload");
    await page.reload();
    await expect(prompt).toHaveValue("Survives reload");
    // Close conversation ends the runtime; typed /quit does not (it detaches a
    // terminal).
    await page
      .locator(".conversation-head")
      .getByLabel("Conversation menu", { exact: true })
      .tap();
    await page.getByRole("menuitem", { name: "Close conversation" }).tap();
    // A closed session keeps its composer: the next message starts it again.
    await expect(prompt).toBeVisible();
    await prompt.fill("/att");
  });
});
