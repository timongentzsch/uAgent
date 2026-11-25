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
  // Its menu makes it a conversation of its own: it leaves the side chats
  // for the list, and keeps the way back.
  await side.getByRole("button", { name: "This side chat" }).click();
  await page
    .getByRole("menuitem", { name: "Open as its own conversation" })
    .click();
  await expect(page).not.toHaveURL(new RegExp(`session=${session.id}$`));
  await expect(page.locator(".transcript")).toContainText(
    "SIDE-FOLLOWUP-COMPLETE",
  );
  await expect(page.locator(".transcript")).not.toContainText(
    "Verified response",
  );
  const sidebar = page.locator(".sidebar");
  await expect(sidebar).toContainText("Side tool probe");
  const head = page.locator(".conversation-head");
  await expect(head.getByRole("button", { name: /^‹/ })).toBeVisible();
  await expect(head.getByRole("button", { name: "Merge" })).toHaveCount(0);
  await head.getByRole("button", { name: /^‹/ }).click();
  await expect(tabs).toHaveText(["Second side question"]);
  await expect(page.locator(".composer .activity-button")).toContainText(
    "1 side chat",
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
  // The newest first; an idle one has nothing to end, only its menu.
  await expect(rows).toHaveText([
    /Second side question\s*Ready/,
    /Side tool probe\s*Ready/,
  ]);
  await expect(rows.getByRole("button", { name: /^Stop/ })).toHaveCount(0);
  await expect(rows.getByRole("button", { name: /^Menu for/ })).toHaveCount(2);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  // A row opens its side chat.
  await rows.last().getByRole("button").click();
  await expect(page).not.toHaveURL(here);
  await expect(transcript).toContainText("SIDE-FOLLOWUP-COMPLETE");
  // Its page starts where it was asked.
  await expect(transcript).not.toContainText("Verified response");
  // Merged, the page stays, saying when; the conversation answers, and
  // its title is its own.
  await expect(merge).toBeEnabled();
  await merge.click();
  await expect(head).toContainText("Merged");
  await expect(transcript).toContainText("Summed up for");
  await expect(transcript).not.toContainText("The person merges");
  await head.getByRole("button", { name: /^‹/ }).click();
  await expect(page).toHaveURL(here);
  await expect(transcript).toContainText("MERGE-HEARD");
  await expect(transcript.locator(".merged-row")).toContainText(
    "Merged side chat · Side tool probe",
  );
  await expect(head.locator("h1")).not.toContainText("merged");
  await expect(activity).toContainText("2 side chats");
  // Neither is in the list of conversations.
  await page.getByRole("button", { name: "Open conversations" }).click();
  await expect(page.locator(".sidebar .session")).toHaveCount(1);
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
  await activity.click();
  await expect(sheet.locator(".activity-row")).toHaveText([/Working/]);
  await page.keyboard.press("Escape");
  release();
  const side = page.getByRole("complementary", { name: "Side chats" });
  await side.getByRole("button", { name: "Allow once", exact: true }).click();
  await expect(side).toContainText("SIDE-TOOL-COMPLETE");
  await activity.click();
  await expect(sheet.locator(".activity-row")).toHaveText([/Ready/]);
  await expect(sheet.getByRole("button", { name: /^Stop .*/ })).toHaveCount(0);
});

test("a side chat is listed as working even when the host's answer is read before the stream delivers its turn", async ({
  page,
  session,
}) => {
  // The stream's updates wait for `release`, once the question has gone out.
  await page.addInitScript(() => {
    const held = [];
    globalThis.holdUpdates = false;
    globalThis.releaseUpdates = () => {
      globalThis.holdUpdates = false;
      for (const deliver of held.splice(0)) deliver();
    };
    const add = EventSource.prototype.addEventListener;
    EventSource.prototype.addEventListener = function (
      type,
      listener,
      ...rest
    ) {
      if (type !== "update") return add.call(this, type, listener, ...rest);
      add.call(this, type, (message) => {
        if (globalThis.holdUpdates) held.push(() => listener(message));
        else listener(message);
      });
    };
  });
  await page.setViewportSize({ width: 1280, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.route("**/api/command", async (route) => {
    const body = route.request().postDataJSON();
    if (body.kind === "submit" && body.session_id !== session.id)
      await page.evaluate(() => (globalThis.holdUpdates = true));
    await route.continue();
  });
  const reply = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/command") &&
      response.request().postDataJSON().kind === "submit" &&
      response.request().postDataJSON().session_id !== session.id,
  );
  await page.getByLabel("Message or guidance").fill("/btw Side tool probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await reply;
  const activity = page.locator(".composer .activity-button");
  await page.evaluate(() => globalThis.releaseUpdates());
  await activity.click();
  await expect(
    page
      .getByRole("dialog", { name: "Activity" })
      .getByRole("button", { name: /^Stop .*/ }),
  ).toBeVisible();
});

test("a side chat's form never reads Send follow-up once it is listed, while its turn runs", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  // Its snapshot is read again after the question is taken, and held.
  const { promise: held, resolve: release } = Promise.withResolvers();
  const { promise: reached, resolve: asked } = Promise.withResolvers();
  let taken = false;
  await page.route("**/api/**", async (route) => {
    const { pathname } = new URL(route.request().url());
    const request = route.request();
    if (request.method() === "POST" && pathname === "/api/command") {
      const body = request.postDataJSON();
      taken ||= body.kind === "submit" && body.session_id !== session.id;
    } else if (
      taken &&
      /^\/api\/sessions\/[a-f0-9]+$/.test(pathname) &&
      !pathname.endsWith(session.id)
    ) {
      asked();
      await held;
    }
    await route.continue();
  });
  await page.getByLabel("Message or guidance").fill("/btw Side tool probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await reached;
  const side = page.getByRole("complementary", { name: "Side chats" });
  // Before its turn is known it is only starting: nothing can be sent.
  await expect(
    side.locator("button[aria-label='Send follow-up']"),
  ).toBeDisabled();
  release();
  await expect(
    side.getByRole("button", { name: "Queue follow-up", exact: true }),
  ).toBeVisible();
});

test("a side chat whose question was taken stays listed when the read of its snapshot after that fails", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  let taken = false;
  let failed = false;
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const { pathname } = new URL(request.url());
    if (request.method() === "POST" && pathname === "/api/command") {
      const body = request.postDataJSON();
      taken ||= body.kind === "submit" && body.session_id !== session.id;
    } else if (
      taken &&
      !failed &&
      /^\/api\/sessions\/[a-f0-9]+$/.test(pathname) &&
      !pathname.endsWith(session.id)
    ) {
      failed = true;
      return route.fulfill({ status: 500, json: { error: "read failed" } });
    }
    await route.continue();
  });
  await page.getByLabel("Message or guidance").fill("/btw Side tool probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  const side = page.getByRole("complementary", { name: "Side chats" });
  await side.getByRole("button", { name: "Allow once", exact: true }).click();
  await expect(side).toContainText("SIDE-TOOL-COMPLETE");
  expect(failed).toBe(true);
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
