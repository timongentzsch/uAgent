import { test, expect, online, selectProbeModel } from "./fixtures.js";

test.beforeEach(({ command, session }) => selectProbeModel(command, session));

for (const width of [320, 1280]) {
  test(`btw side chats and their follow-ups stay beside the parent (${width})`, async ({
    page,
    session,
  }, testInfo) => {
    await page.setViewportSize({ width, height: 844 });
    await page.goto(`/#session=${session.id}`);
    await online(page);
    const prompt = page.getByLabel("Message or guidance");
    await prompt.fill("/btw Side tool probe");
    await page.getByRole("button", { name: "Send", exact: true }).click();
    const side = page.getByRole("complementary", { name: "Side chats" });
    await expect(
      side.getByRole("button", { name: "Allow once", exact: true }),
    ).toBeVisible();
    await side.getByRole("button", { name: "Allow once", exact: true }).click();
    await expect(side).toContainText("SIDE-TOOL-COMPLETE");
    await expect(page.locator(".transcript")).not.toContainText(
      "SIDE-TOOL-COMPLETE",
    );
    await page.reload();
    await online(page);
    await expect(side).toContainText("SIDE-TOOL-COMPLETE");
    await side.getByLabel("Follow up on side chat").fill("Side followup probe");
    if (width === 1280)
      await side.getByLabel("Follow up on side chat").press("Enter");
    else
      await side
        .getByRole("button", { name: "Send follow-up", exact: true })
        .click();
    await expect(side).toContainText("SIDE-FOLLOWUP-COMPLETE");
    await expect(side).toContainText("SIDE-TOOL-COMPLETE");
    // Another /btw starts a side chat of its own; neither reaches the parent.
    const tabs = side.getByRole("tab");
    await expect(tabs).toHaveCount(1);
    const sent = page.waitForRequest((request) => {
      if (!request.url().endsWith("/api/command")) return false;
      const body = request.postDataJSON();
      return (
        body.text === "Second side question" && body.session_id !== session.id
      );
    });
    await prompt.fill("/btw Second side question");
    await page.getByRole("button", { name: "Send", exact: true }).click();
    await sent;
    await expect(prompt).toHaveValue("");
    await expect(tabs).toHaveCount(2);
    await expect(tabs.last()).toHaveAttribute("aria-selected", "true");
    await expect(side).toContainText("Verified response");
    await expect(side).not.toContainText("SIDE-TOOL-COMPLETE");
    await expect(page.locator(".transcript")).not.toContainText(
      "Verified response",
    );
    await expect(page).toHaveURL(new RegExp(`session=${session.id}$`));
    // Its turn over: under the transcript the box grows with the answer, and
    // a tab is no still thing to click until then.
    await expect(
      side.getByRole("button", { name: "Send follow-up", exact: true }),
    ).toBeVisible();
    await tabs.first().click();
    await expect(side).toContainText("SIDE-FOLLOWUP-COMPLETE");
    const layout = await side.evaluate((node) => ({
      overflow: document.documentElement.scrollWidth > innerWidth,
      left: node.getBoundingClientRect().left,
      width: node.getBoundingClientRect().width,
      height: node.getBoundingClientRect().height,
      chat: document.querySelector(".transcript").getBoundingClientRect().right,
      button: node
        .querySelector('button[type="submit"]')
        .getBoundingClientRect().height,
    }));
    expect(layout.overflow).toBe(false);
    expect(layout.width).toBeLessThanOrEqual(width);
    if (width === 320) {
      // Under the transcript, with touch targets.
      expect(layout.height).toBeLessThan(500);
      expect(layout.button).toBeGreaterThanOrEqual(44);
    } else {
      // A column beside the chat.
      expect(layout.left).toBeGreaterThanOrEqual(layout.chat);
    }
    await page.screenshot({ path: testInfo.outputPath("btw.png") });
    await side.getByRole("button", { name: "Open side chat" }).click();
    await expect(page).not.toHaveURL(new RegExp(`session=${session.id}$`));
    await expect(page.locator(".transcript")).toContainText(
      "SIDE-FOLLOWUP-COMPLETE",
    );
  });
}

test("a side chat asked while the conversation is still waking starts once it is up", async ({
  command,
  session,
}) => {
  // Its runtime stops, as after an idle spell, and is asked the moment it
  // is started again.
  const target = { session_id: session.id, generation: session.generation };
  await command("close", target);
  const { session: woken } = await command("activate", {
    session_id: session.id,
  });
  const made = await command("side", {
    session_id: woken.id,
    generation: woken.generation,
    operation: "create",
    text: "Asked while waking",
  });
  expect(made.error || "").toBe("");
  expect(made.result.id).toMatch(/^[a-f0-9]{16,64}$/);
});
