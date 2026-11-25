import { test, expect, online, selectProbeModel } from "./fixtures.js";
import { roomy } from "./roomy.js";

test.beforeEach(({ command, session }) => selectProbeModel(command, session));

const ask = async (page, question) => {
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill(`/btw ${question}`);
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(prompt).toHaveValue("");
};

const parts = (page) => ({
  side: page.getByRole("complementary", { name: "Side chats" }),
  tabs: page
    .getByRole("complementary", { name: "Side chats" })
    .getByRole("tab"),
  activity: page.locator(".composer .activity-button"),
  rows: page.getByRole("dialog", { name: "Activity" }).locator(".activity-row"),
  toggle: page
    .locator(".conversation-head")
    .getByRole("button", { name: /^Side chats/ }),
});

// Picks an item of a side chat's menu: the one in front, or `row`'s.
const pick = async (page, item, row) => {
  await (row ?? page.getByRole("complementary", { name: "Side chats" }))
    .getByRole("button", { name: row ? /^Menu for/ : "This side chat" })
    .click();
  await page.getByRole("menuitem", { name: item }).click();
};

// Deletes it through its menu, after the confirmation.
const remove = async (page, question, row) => {
  await pick(page, "Delete side chat…", row);
  const dialog = page.getByRole("dialog", { name: "Delete this side chat?" });
  await expect(dialog).toContainText(
    `“${question}” and its answers are removed for good. What was merged into the conversation stays.`,
  );
  await dialog.getByRole("button", { name: "Delete", exact: true }).click();
  await expect(dialog).toHaveCount(0);
};

test("a side chat asked on one device is one on another, shown there when asked for", async ({
  browser,
  page,
  session,
  storageState,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await ask(page, "Second side question");
  const here = parts(page);
  await expect(here.side).toContainText("Verified response");

  const other = await (await browser.newContext({ storageState })).newPage();
  await other.setViewportSize({ width: 1440, height: 900 });
  await other.goto(`/#session=${session.id}`);
  await online(other);
  const there = parts(other);
  // Counted and listed there, not shown, and no conversation of the list.
  await expect(there.activity).toContainText("1 side chat");
  await expect(there.side).toHaveCount(0);
  await expect(other.locator(".sidebar")).not.toContainText(
    "Second side question",
  );
  await there.activity.click();
  await expect(there.rows).toHaveText([/Second side question\s*Ready/]);
  await there.rows.getByRole("button").first().click();
  await expect(there.tabs).toHaveText(["Second side question"]);
  await expect(there.side).toContainText("Verified response");
  await roomy(other);
  // Hiding it there leaves it shown here.
  await there.side.getByRole("button", { name: "Hide side chats" }).click();
  await expect(there.side).toHaveCount(0);
  await expect(here.tabs).toHaveText(["Second side question"]);
  // Deleted here, it is gone there too, without an error.
  await remove(page, "Second side question");
  await expect(here.side).toHaveCount(0);
  await expect(there.toggle).toHaveCount(0);
  await expect(there.activity).toHaveCount(0);
  await expect(other.getByRole("alert")).toHaveCount(0);
  await other.context().close();
});

test("side chats that do not fit are tabs that scroll, the shown one in view", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const { side, tabs, activity } = parts(page);
  const asked = [1, 2, 3, 4, 5].map(
    (n) => `Side question number ${n} with a long name`,
  );
  for (const [n, question] of asked.entries()) {
    await ask(page, question);
    await expect(tabs).toHaveCount(n + 1);
    await expect(tabs.last()).toHaveAttribute("aria-selected", "true");
    await expect(side.locator(".message.response")).toContainText(
      "Native worker",
    );
  }
  await expect(activity).toContainText("5 side chats");
  const strip = side.getByRole("tablist");
  expect(
    await strip.evaluate((node) => node.scrollWidth > node.clientWidth),
  ).toBe(true);
  // The shown tab is within the strip, wherever that is scrolled to.
  const inView = (tab) =>
    tab.evaluate((node) => {
      const strip = node.closest('[role="tablist"]').getBoundingClientRect();
      const box = node.getBoundingClientRect();
      return box.left >= strip.left - 1 && box.right <= strip.right + 1;
    });
  await expect.poll(() => inView(tabs.last())).toBe(true);
  await tabs.first().click();
  await expect.poll(() => inView(tabs.first())).toBe(true);
  await expect(page).toHaveURL(new RegExp(`session=${session.id}$`));
  await roomy(page);
});

test("a side chat opened from anywhere is shown as one, its tab in front", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await ask(page, "Second side question");
  const { side, tabs } = parts(page);
  await expect(side).toContainText("Verified response");
  await side.getByRole("button", { name: "Hide side chats" }).click();
  await expect(side).toHaveCount(0);
  // Its own address, as a notification or Search opens it.
  const sideId = await page.evaluate(async (parent) => {
    const listed = await (await fetch("/api/sessions")).json();
    return listed.sessions.find((item) => item.parent === parent).id;
  }, session.id);
  await page.evaluate((target) => {
    location.hash = `session=${target}`;
  }, sideId);
  await expect(page).toHaveURL(new RegExp(`session=${session.id}$`));
  await expect(tabs).toHaveText(["Second side question"]);
  await expect(tabs.first()).toHaveAttribute("aria-selected", "true");
});

test("the header shows and hides the whole column; a side chat goes only when deleted", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const { side, tabs, activity, rows, toggle } = parts(page);
  await expect(toggle).toHaveCount(0);
  await ask(page, "First side question");
  await expect(side).toContainText("Verified response");
  await ask(page, "Second side question");
  await expect(tabs).toHaveCount(2);
  await expect(side.locator(".message.response")).toContainText(
    "Native worker",
  );
  await expect(toggle).toHaveAccessibleName("Side chats, 2");
  await expect(toggle).toHaveAttribute("aria-pressed", "true");
  await roomy(page);
  // The panel button hides the column; the header shows it as it was.
  await side.getByRole("button", { name: "Hide side chats" }).click();
  await expect(side).toHaveCount(0);
  await expect(toggle).toHaveAttribute("aria-pressed", "false");
  await expect(activity).toContainText("2 side chats");
  await roomy(page);
  await toggle.click();
  await expect(tabs).toHaveText([
    "First side question",
    "Second side question",
  ]);
  await expect(tabs.last()).toHaveAttribute("aria-selected", "true");
  // Hidden stays hidden on this device after a reload.
  await toggle.click();
  await expect(side).toHaveCount(0);
  await page.reload();
  await online(page);
  await expect(toggle).toHaveAttribute("aria-pressed", "false");
  await expect(side).toHaveCount(0);
  // A question asked while hidden shows the column, its tab in front.
  await ask(page, "Third side question");
  await expect(tabs).toHaveCount(3);
  await expect(tabs.last()).toHaveAttribute("aria-selected", "true");
  await expect(side.locator(".message.response")).toContainText(
    "Native worker",
  );
  // Closing every tab hides the column; the header opens the newest.
  for (const _ of [1, 2, 3]) await pick(page, "Close tab");
  await expect(side).toHaveCount(0);
  await expect(toggle).toHaveAccessibleName("Side chats, 3");
  await toggle.click();
  await expect(tabs).toHaveText(["Third side question"]);
  // A row of the Activity list shows the column with its tab in front.
  await toggle.click();
  await activity.click();
  await rows
    .filter({ hasText: "First side question" })
    .getByRole("button")
    .first()
    .click();
  // Tabs keep the order asked.
  await expect(tabs).toHaveText(["First side question", "Third side question"]);
  await expect(tabs.first()).toHaveAttribute("aria-selected", "true");
  // Delete asks first; Cancel keeps it.
  await pick(page, "Delete side chat…");
  await page
    .getByRole("dialog", { name: "Delete this side chat?" })
    .getByRole("button", { name: "Cancel" })
    .click();
  await expect(tabs).toHaveCount(2);
  await remove(page, "First side question");
  await expect(tabs).toHaveText(["Third side question"]);
  await expect(toggle).toHaveAccessibleName("Side chats, 2");
  await expect(activity).toContainText("2 side chats");
  // A hidden one is deleted from its row, without opening it.
  await activity.click();
  await remove(
    page,
    "Second side question",
    rows.filter({ hasText: "Second side question" }),
  );
  await expect(activity).toContainText("1 side chat");
  await page.keyboard.press("Escape");
  await expect(tabs).toHaveText(["Third side question"]);
  await roomy(page);
});

// Says "Release the summary" in a conversation of its own, which lets the
// probe write a held side chat's summary.
const releaseSummary = async (command, host) => {
  const { session } = await command("create", { cwd: host.project });
  const { session: active } = await command("activate", {
    session_id: session.id,
  });
  await selectProbeModel(command, active);
  await command("submit", {
    session_id: active.id,
    generation: active.generation,
    text: "Release the summary",
  });
};

test("Merge sums up in view, then the conversation gets one row and answers", async ({
  page,
  session,
  command,
  host,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const transcript = page.locator(".transcript").first();
  const title = await page.locator(".conversation-head h1").textContent();
  await ask(page, "Held summary probe");
  const { side, tabs } = parts(page);
  await expect(side).toContainText("Native worker");
  await side.getByRole("button", { name: "Merge", exact: true }).click();
  // Summing up: the tab stays, working, and both sides say so.
  const summing = side.getByRole("button", { name: "Summing up…" });
  await expect(summing).toBeDisabled();
  await expect(tabs.first().locator(".status-led.running")).toHaveCount(1);
  await expect(side).toContainText("Summing up for");
  const pending = transcript.locator('[data-message-id^="summing-"]');
  await expect(pending).toContainText("is summing up");
  await roomy(page);
  // Hidden, it still says so in the conversation.
  await side.getByRole("button", { name: "Hide side chats" }).click();
  await expect(side).toHaveCount(0);
  await expect(pending).toHaveCount(1);
  await releaseSummary(command, host);
  // One row, folded like a step, with the summary; then the answer.
  const row = transcript.locator(".merged-row");
  await expect(row).toHaveCount(1);
  await expect(row).toContainText("Merged side chat · Held summary probe");
  await expect(row).toContainText("SIDE-SUMMARY: the table has two columns.");
  await expect(pending).toHaveCount(0);
  await expect(transcript).toContainText("MERGE-HEARD");
  await row.getByRole("button", { name: /Merged side chat/ }).click();
  await expect(row).not.toContainText("SIDE-SUMMARY");
  const { activity, rows } = parts(page);
  await activity.click();
  await expect(rows).toHaveText([/Held summary probe\s*Ready · merged/]);
  await page.keyboard.press("Escape");
  // The conversation's title is its own, never the report's.
  await expect(page.locator(".conversation-head h1")).toHaveText(title);
  await expect(page.locator(".conversation-head h1")).not.toContainText(
    "merged",
  );
  // Show side chat brings the tab back: summed up, and saying when.
  await row.getByRole("button", { name: "Show side chat" }).click();
  await expect(tabs).toHaveText(["Held summary probe"]);
  await expect(side).toContainText("Summed up for");
  await expect(side).toContainText("SIDE-SUMMARY");
  await expect(side).not.toContainText("The person merges");
  await expect(side.locator(".merged-at")).toContainText("Merged");
  await expect(side.getByRole("button", { name: "Merge" })).toHaveCount(0);
  await roomy(page);
  // A further answer offers Merge again.
  const follow = side.getByLabel("Follow up on side chat");
  await follow.fill("Anything else?");
  await follow.press("Enter");
  await expect(
    side.getByRole("button", { name: "Merge", exact: true }),
  ).toBeEnabled();
  await expect(side.locator(".merged-at")).toHaveCount(0);
  // Deleted, its summary stays in the conversation, with no way to it.
  await remove(page, "Held summary probe");
  await expect(side).toHaveCount(0);
  await expect(row).toContainText("Merged side chat · Held summary probe");
  await expect(row.getByRole("button", { name: "Show side chat" })).toHaveCount(
    0,
  );
  await expect(activity).toHaveCount(0);
});

test("a merge that is stopped leaves the side chat as it was and says why", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await ask(page, "Held summary probe");
  const { side } = parts(page);
  await expect(side).toContainText("Native worker");
  await side.getByRole("button", { name: "Merge", exact: true }).click();
  await expect(side.getByRole("button", { name: "Summing up…" })).toBeVisible();
  await side.getByRole("button", { name: "Stop", exact: true }).click();
  await expect(side.locator(".merge-error")).toContainText("Not merged");
  await expect(
    side.getByRole("button", { name: "Merge", exact: true }),
  ).toBeEnabled();
  await expect(page.locator(".transcript").first()).not.toContainText(
    "is summing up",
  );
  await expect(page.locator(".merged-row")).toHaveCount(0);
});

test.describe("phone", () => {
  test.use({
    isMobile: true,
    hasTouch: true,
    viewport: { width: 390, height: 844 },
  });

  test("on a phone a side chat is a page with the way back, opened from the list", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    await online(page);
    const here = new RegExp(`session=${session.id}$`);
    await ask(page, "Second side question");
    await expect(page).not.toHaveURL(here);
    const transcript = page.locator(".transcript");
    await expect(transcript).toContainText("Verified response");
    const head = page.locator(".conversation-head");
    await expect(head.getByRole("button", { name: "Merge" })).toBeEnabled();
    await roomy(page);
    await head.getByRole("button", { name: /^‹/ }).click();
    await expect(page).toHaveURL(here);
    await expect(page.getByRole("complementary")).toHaveCount(0);
    const { activity, rows } = parts(page);
    await activity.click();
    await rows.getByRole("button").first().click();
    await expect(page).not.toHaveURL(here);
    // The page's menu opens it as a conversation of its own.
    await head.getByRole("button", { name: "Conversation menu" }).click();
    await page
      .getByRole("menuitem", { name: "Open as its own conversation" })
      .click();
    await expect(head.getByRole("button", { name: "Merge" })).toHaveCount(0);
    await expect(head.getByRole("button", { name: /^‹/ })).toBeVisible();
    await expect(page.getByRole("button", { name: /^Side chats/ })).toHaveCount(
      0,
    );
    await page.getByRole("button", { name: "Open conversations" }).click();
    await expect(page.locator(".sidebar")).toContainText(
      "Second side question",
    );
  });
  test("on a phone a side chat's page deletes it, then goes back", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    await online(page);
    const here = new RegExp(`session=${session.id}$`);
    await ask(page, "Second side question");
    await expect(page).not.toHaveURL(here);
    await expect(page.locator(".transcript")).toContainText("Native worker");
    const head = page.locator(".conversation-head");
    await expect(page.getByRole("button", { name: /^Side chats/ })).toHaveCount(
      0,
    );
    await head.getByRole("button", { name: "Conversation menu" }).click();
    // The last item, after a line; More holds no second Delete.
    const menu = page.getByRole("menu", { name: "Conversation menu" });
    await expect(menu.locator(":scope > *").last()).toHaveText(
      "Delete side chat…",
    );
    await expect(menu.locator("hr")).toHaveCount(1);
    await roomy(page);
    await menu.getByRole("menuitem", { name: "More" }).click();
    await expect(
      menu.getByRole("menuitem", { name: "Delete", exact: true }),
    ).toHaveCount(0);
    await page.keyboard.press("ArrowLeft");
    await menu.getByRole("menuitem", { name: "Delete side chat…" }).click();
    const dialog = page.getByRole("dialog", { name: "Delete this side chat?" });
    await expect(dialog).toContainText("“Second side question”");
    await roomy(page);
    await dialog.getByRole("button", { name: "Delete", exact: true }).click();
    await expect(page).toHaveURL(here);
    await expect(page.locator(".composer .activity-button")).toHaveCount(0);
    await expect(page.getByRole("alert")).toHaveCount(0);
  });
});
