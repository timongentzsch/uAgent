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
});

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
  await there.rows.getByRole("button").click();
  await expect(there.tabs).toHaveText(["Second side question"]);
  await expect(there.side).toContainText("Verified response");
  await roomy(other);
  // Hiding it there leaves it shown here.
  await there.side.getByRole("button", { name: "Hide side chat" }).click();
  await expect(there.side).toHaveCount(0);
  await expect(here.tabs).toHaveText(["Second side question"]);
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
  await side.getByRole("button", { name: "Hide side chat" }).click();
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
  await side.getByRole("button", { name: "Hide side chat" }).click();
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
    await rows.getByRole("button").click();
    await expect(page).not.toHaveURL(here);
    // The arrow's action is in the page's menu: it opens there as a
    // conversation of its own.
    await head.getByRole("button", { name: "Conversation menu" }).click();
    await page
      .getByRole("menuitem", { name: "Open as a conversation of its own" })
      .click();
    await expect(head.getByRole("button", { name: "Merge" })).toHaveCount(0);
    await expect(head.getByRole("button", { name: /^‹/ })).toBeVisible();
    await page.getByRole("button", { name: "Open conversations" }).click();
    await expect(page.locator(".sidebar")).toContainText(
      "Second side question",
    );
  });
});
