// Text selected in a transcript or a file is asked about in a side chat, or
// added to the composer as a quote.
import {
  test,
  expect,
  online,
  openFiles,
  selectProbeModel,
  writeFiles,
} from "./fixtures.js";

test.beforeEach(({ command, session }) => selectProbeModel(command, session));

// Selects `text` where it first appears under the locator.
const select = (locator, text) =>
  locator.evaluate((root, text) => {
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      const start = node.data.indexOf(text);
      if (start < 0) continue;
      const range = document.createRange();
      range.setStart(node, start);
      range.setEnd(node, start + text.length);
      getSelection().removeAllRanges();
      getSelection().addRange(range);
      return;
    }
    throw new Error(`"${text}" is not there`);
  }, text);

// The conversation with one answer in it.
async function answered(page, session, viewport) {
  await page.setViewportSize(viewport);
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.getByLabel("Message or guidance").fill("Hello");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  const transcript = page.locator(".transcript");
  await expect(transcript).toContainText("Native worker");
  await expect(
    page.getByRole("button", { name: "Send", exact: true }),
  ).toBeVisible();
  return transcript;
}

const box = (locator) =>
  locator.evaluate((node) => node.getBoundingClientRect().toJSON());
const fits = (page) =>
  page.evaluate(() => document.documentElement.scrollWidth <= innerWidth);

test("a selection in an answer is asked about in side chats beside it", async ({
  page,
  session,
}) => {
  const transcript = await answered(page, session, {
    width: 1440,
    height: 900,
  });
  const messages = await transcript.locator(".message").count();
  const actions = page.getByRole("group", { name: "Selection" });
  await expect(actions).toHaveCount(0);
  await select(transcript, "answer with a table");
  await expect(actions.getByRole("button")).toHaveText([
    "Ask in side chat",
    "Add to chat",
  ]);
  // The buttons sit under the end of the selection.
  const end = await page.evaluate(() => {
    const rects = getSelection().getRangeAt(0).getClientRects();
    return rects[rects.length - 1].toJSON();
  });
  const placed = await box(actions);
  expect(placed.top).toBeGreaterThanOrEqual(end.bottom);
  expect(Math.abs(placed.right - end.right)).toBeLessThan(2);

  await actions.getByRole("button", { name: "Ask in side chat" }).click();
  const side = page.getByRole("complementary", { name: "Side chats" });
  const tabs = side.getByRole("tab");
  await expect(tabs).toHaveText(["New side chat"]);
  await expect(side.locator(".side-quote")).toContainText(
    "answer with a table",
  );
  const question = side.getByLabel("Ask in a side chat");
  await expect(question).toBeFocused();
  await expect(actions).toHaveCount(0);
  // The passage stays marked while its question is written.
  const marked = () => page.evaluate(() => CSS.highlights.has("asked"));
  expect(await marked()).toBe(true);
  // A column between the chat and the window's edge.
  expect((await box(side)).left).toBeGreaterThanOrEqual(
    (await box(transcript)).right,
  );
  await question.fill("Which table?");
  await question.press("Enter");
  await expect(tabs).toHaveText(["Which table?"]);
  await expect.poll(marked).toBe(false);
  const asked = side.locator(".message.user");
  await expect(asked.locator("blockquote")).toHaveText("answer with a table");
  await expect(asked).toContainText("Which table?");
  await expect(side.locator(".message.response")).toContainText(
    "Native worker",
  );
  await expect(transcript.locator(".message")).toHaveCount(messages);

  // Escape leaves a question unasked; nothing was started.
  await select(transcript, "streamed");
  await actions.getByRole("button", { name: "Ask in side chat" }).click();
  await expect(tabs).toHaveText(["Which table?", "New side chat"]);
  // Without its quote the question can still be asked.
  await side.getByRole("button", { name: "Remove the quote" }).click();
  await expect(side.locator(".side-quote")).toHaveCount(0);
  await expect(tabs).toHaveText(["Which table?", "New side chat"]);
  await side.getByLabel("Ask in a side chat").press("Escape");
  await expect(tabs).toHaveText(["Which table?"]);
  await expect(side.locator(".message.response")).toBeVisible();

  // Each ask is a side chat of its own.
  await select(transcript, "streamed");
  await actions.getByRole("button", { name: "Ask in side chat" }).click();
  await side.getByLabel("Ask in a side chat").fill("Streamed how?");
  await side.getByRole("button", { name: "Ask", exact: true }).click();
  await expect(tabs).toHaveText(["Which table?", "Streamed how?"]);
  await expect(tabs.last()).toHaveAttribute("aria-selected", "true");
  await expect(side.locator(".message.user blockquote")).toHaveText("streamed");
  await expect(side.locator(".message.response")).toContainText(
    "Native worker",
  );

  // A selection in a side chat's own answer follows up in that chat.
  await select(side.locator(".message.response"), "Native worker");
  await actions.getByRole("button", { name: "Ask in side chat" }).click();
  await expect(tabs).toHaveCount(2);
  const follow = side.getByLabel("Follow up on side chat");
  await expect(follow).toBeFocused();
  await expect(side.locator(".side-quote")).toContainText("Native worker");
  await follow.fill("Ready for what?");
  await follow.press("Enter");
  await expect(side.locator(".message.user")).toHaveCount(2);
  await expect(side.locator(".message.user blockquote").last()).toHaveText(
    "Native worker",
  );
  await expect(side.locator(".side-quote")).toHaveCount(0);
  await expect(tabs).toHaveCount(2);
  await expect(transcript.locator(".message")).toHaveCount(messages);
  expect(await fits(page)).toBe(true);

  await page.reload();
  await online(page);
  await expect(tabs).toHaveText(["Which table?", "Streamed how?"]);
  await side.getByRole("button", { name: "Close side chat" }).click();
  await expect(tabs).toHaveText(["Which table?"]);
  await side.getByRole("button", { name: "Close side chat" }).click();
  await expect(side).toHaveCount(0);
});

test("Add to chat puts the selection in the composer as a quote", async ({
  page,
  session,
}) => {
  const transcript = await answered(page, session, {
    width: 1440,
    height: 900,
  });
  await select(transcript, "answer with a table");
  await page
    .getByRole("group", { name: "Selection" })
    .getByRole("button", { name: "Add to chat" })
    .click();
  const composer = page.getByLabel("Message or guidance");
  await expect(composer).toHaveValue("> answer with a table");
  await expect(composer).toBeFocused();
  await expect(page.getByRole("group", { name: "Selection" })).toHaveCount(0);
  await expect(
    page.getByRole("complementary", { name: "Side chats" }),
  ).toHaveCount(0);
});

const pickLines = (pane) =>
  pane.locator(".code-view").evaluate((view) => {
    const lines = view.querySelectorAll(".line");
    const range = document.createRange();
    range.setStart(lines[0].firstChild, 0);
    range.setEnd(lines[1].firstChild, 1);
    getSelection().removeAllRanges();
    getSelection().addRange(range);
  });

for (const [width, layout] of [
  [1440, "three columns"],
  [1100, "the side chats under the transcript"],
]) {
  test(`lines of a file are asked about with Files open: ${layout}`, async ({
    page,
    session,
    host,
  }) => {
    await writeFiles(host.project, {
      "src/app.ts": "// One.\nexport const two = 2;\n",
    });
    const pane = await openFiles(page, session);
    await page.setViewportSize({ width, height: 900 });
    const list = pane.getByRole("navigation", { name: "Folder" });
    await list.getByRole("button", { name: "src" }).click();
    await list.getByRole("button", { name: "app.ts" }).click();
    await pickLines(pane);
    await pane.getByRole("button", { name: "Ask in side chat" }).click();
    const side = page.getByRole("complementary", { name: "Side chats" });
    await expect(side.locator(".side-quote")).toContainText("src/app.ts:1-2");
    await expect(side.locator(".side-quote")).toContainText("// One.");
    await expect(side.getByLabel("Ask in a side chat")).toBeFocused();
    const [chat, chats, files] = await Promise.all(
      [page.locator(".side-chat"), side, pane].map(box),
    );
    if (width === 1440) {
      expect(chats.left).toBeGreaterThanOrEqual(chat.right);
      expect(files.left).toBeGreaterThanOrEqual(chats.right - 1);
      expect(chat.width).toBeGreaterThanOrEqual(360);
    } else {
      expect(chats.right).toBeLessThanOrEqual(chat.right);
      expect(files.left).toBeGreaterThanOrEqual(chat.right - 1);
    }
    expect(await fits(page)).toBe(true);
  });
}

test("on a phone the actions are a bar above the composer and the side chats sit under the transcript", async ({
  page,
  session,
}) => {
  const transcript = await answered(page, session, { width: 390, height: 844 });
  await select(transcript, "answer with a table");
  const actions = page.getByRole("group", { name: "Selection" });
  const composer = page.locator(".composer");
  const bar = await box(actions);
  expect(bar.bottom).toBeLessThanOrEqual((await box(composer)).top);
  await expect(actions).toContainText("answer with a table");
  for (const button of await actions.getByRole("button").all())
    expect((await box(button)).height).toBeGreaterThanOrEqual(44);
  await actions.getByRole("button", { name: "Ask in side chat" }).click();
  const side = page.getByRole("complementary", { name: "Side chats" });
  await side.getByLabel("Ask in a side chat").fill("Which table?");
  await side.getByRole("button", { name: "Ask", exact: true }).click();
  await expect(side.locator(".message.response")).toContainText(
    "Native worker",
  );
  const [above, chats, below] = await Promise.all(
    [transcript, side, composer].map(box),
  );
  expect(chats.top).toBeGreaterThanOrEqual(above.bottom - 1);
  expect(chats.bottom).toBeLessThanOrEqual(below.top);
  expect(await fits(page)).toBe(true);
});
