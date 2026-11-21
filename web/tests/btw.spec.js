import { test, expect, online, selectProbeModel } from "./fixtures.js";

test.beforeEach(({ command, session }) => selectProbeModel(command, session));

test("btw side chats and their follow-ups stay beside the parent", async ({
  page,
  session,
}, testInfo) => {
  await page.setViewportSize({ width: 1280, height: 844 });
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
  await side.getByLabel("Follow up on side chat").press("Enter");
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
    button: node.querySelector('button[type="submit"]').getBoundingClientRect()
      .height,
  }));
  expect(layout.overflow).toBe(false);
  // A column beside the chat.
  expect(layout.left).toBeGreaterThanOrEqual(layout.chat);
  await page.screenshot({ path: testInfo.outputPath("btw.png") });
  await side.getByRole("button", { name: "Open side chat" }).click();
  await expect(page).not.toHaveURL(new RegExp(`session=${session.id}$`));
  await expect(page.locator(".transcript")).toContainText(
    "SIDE-FOLLOWUP-COMPLETE",
  );
});

test("on a phone a side chat is a page of its own, listed in its conversation", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 320, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  const send = page.getByRole("button", { name: "Send", exact: true });
  const here = new RegExp(`session=${session.id}$`);
  const transcript = page.locator(".transcript");
  await prompt.fill("/btw Side tool probe");
  await send.click();
  // The side chat opens in place of the conversation, with the way back.
  await expect(page).not.toHaveURL(here);
  await page.getByRole("button", { name: "Allow once", exact: true }).click();
  await expect(transcript).toContainText("SIDE-TOOL-COMPLETE");
  const head = page.locator(".conversation-head");
  const merge = head.getByRole("button", { name: "Merge", exact: true });
  await expect(merge).toBeEnabled();
  // One input: a follow-up is an ordinary message there.
  await expect(page.getByRole("complementary")).toHaveCount(0);
  await prompt.fill("Side followup probe");
  await send.click();
  await expect(transcript).toContainText("SIDE-FOLLOWUP-COMPLETE");
  await head.getByRole("button", { name: /^‹/ }).click();
  await expect(page).toHaveURL(here);
  await expect(transcript).not.toContainText("SIDE-TOOL-COMPLETE");
  // The conversation's side chats are in its Activity list, with what its
  // commands and agents are.
  const activity = page.locator(".composer .activity-button");
  const rows = page
    .getByRole("dialog", { name: "Activity" })
    .locator(".activity-row");
  await expect(activity).toContainText("1 side chat");
  await page.reload();
  await online(page);
  await expect(activity).toContainText("1 side chat");
  await prompt.fill("/btw Second side question");
  await send.click();
  await expect(page).not.toHaveURL(here);
  await expect(transcript).toContainText("Native worker");
  await expect(merge).toBeEnabled();
  await head.getByRole("button", { name: /^‹/ }).click();
  await expect(activity).toContainText("2 side chats");
  await activity.click();
  await expect(rows).toHaveText([/Side tool probe/, /Second side question/]);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  // A row opens its side chat.
  await rows.first().getByRole("button").first().click();
  await expect(page).not.toHaveURL(here);
  await expect(transcript).toContainText("SIDE-FOLLOWUP-COMPLETE");
  // Merged, a side chat leaves the list and its conversation answers.
  await expect(merge).toBeEnabled();
  await merge.click();
  await expect(page).toHaveURL(here);
  await expect(transcript).toContainText("MERGE-HEARD");
  await expect(activity).toContainText("1 side chat");
  // Removed from the list, a side chat stays a conversation.
  await activity.click();
  await rows
    .getByRole("button", { name: /^Remove .* from the side chats$/ })
    .click();
  await expect(activity).toHaveCount(0);
});

test("a side chat is listed as working from the moment its question is taken until its turn ends", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  // Its question is held back on the way to the host.
  const { promise: held, resolve: release } = Promise.withResolvers();
  const { promise: reached, resolve: asked } = Promise.withResolvers();
  await page.route("**/api/command", async (route) => {
    const body = route.request().postDataJSON();
    if (body.kind !== "submit" || body.session_id === session.id)
      return route.continue();
    asked();
    await held;
    await route.continue();
  });
  await page.getByLabel("Message or guidance").fill("/btw Side tool probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await reached;
  const activity = page.locator(".composer .activity-button");
  const sheet = page.getByRole("dialog", { name: "Activity" });
  await expect(activity).toHaveCount(0);
  release();
  await activity.click();
  await expect(sheet.getByRole("button", { name: /^Stop .*/ })).toBeVisible();
  await page.keyboard.press("Escape");
  const side = page.getByRole("complementary", { name: "Side chats" });
  await side.getByRole("button", { name: "Allow once", exact: true }).click();
  await expect(side).toContainText("SIDE-TOOL-COMPLETE");
  await activity.click();
  await expect(
    sheet.getByRole("button", { name: /^Remove .* from the side chats$/ }),
  ).toBeVisible();
});

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
