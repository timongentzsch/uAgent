// Text selected in a transcript or a file is asked about in a side chat, or
// quoted in the draft.
import {
  test,
  expect,
  online,
  openFiles,
  leaveEnd,
  savedConversation,
  selectProbeModel,
  writeFiles,
} from "./fixtures.js";
import { uneven } from "./insets.js";

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
    "Quote",
  ]);
  expect(await uneven(page)).toEqual([]);
  // The buttons sit under the selection: shorter than they are, it has
  // them start where it starts.
  const end = await page.evaluate(() => {
    const rects = getSelection().getRangeAt(0).getClientRects();
    return rects[rects.length - 1].toJSON();
  });
  const placed = await box(actions);
  expect(placed.top).toBeGreaterThanOrEqual(end.bottom);
  expect(placed.width).toBeGreaterThan(end.width);
  expect(Math.abs(placed.left - end.left)).toBeLessThan(2);

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

test("the selection's buttons stay with the words selected", async ({
  page,
  session,
}) => {
  const transcript = await answered(page, session, {
    width: 1440,
    height: 900,
  });
  const actions = page.getByRole("group", { name: "Selection" });
  // From `from` in the heading to `offset` in the text `to` (its end if -1).
  const reach = (from, to, offset) =>
    transcript.evaluate(
      (root, [from, to, offset]) => {
        const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
        const text = (words) => {
          walker.currentNode = root;
          while (walker.nextNode())
            if (walker.currentNode.data === words) return walker.currentNode;
          throw new Error(`"${words}" is not there`);
        };
        const last = text(to);
        const range = document.createRange();
        range.setStart(text("Verified response"), from);
        range.setEnd(last, offset < 0 ? last.length : offset);
        getSelection().removeAllRanges();
        getSelection().addRange(range);
        const boxes = [...range.getClientRects()].filter((box) => box.width);
        return {
          left: Math.min(...boxes.map((box) => box.left)),
          right: boxes.at(-1).right,
          bottom: boxes.at(-1).bottom,
          next: last.parentElement.getBoundingClientRect().top,
        };
      },
      [from, to, offset],
    );
  // Longer than the buttons: they end where it ends.
  const long = await reach(0, " answer with a table.", -1);
  await expect
    .poll(async () => Math.abs((await box(actions)).right - long.right))
    .toBeLessThan(2);
  expect((await box(actions)).top).toBeGreaterThanOrEqual(long.bottom);
  // Ending at the start of the next block, it ends in the heading: the
  // buttons stay under the heading's words, not under the block after.
  const heading = await reach(9, "A ", 0);
  await expect
    .poll(async () => (await box(actions)).top)
    .toBeLessThan(heading.next);
  expect(Math.abs((await box(actions)).left - heading.left)).toBeLessThan(2);
});

test("Merge sums a side chat up for the conversation, which answers, and closes it", async ({
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
    .getByRole("button", { name: "Ask in side chat" })
    .click();
  const side = page.getByRole("complementary", { name: "Side chats" });
  await side.getByLabel("Ask in a side chat").fill("Which table?");
  await side.getByLabel("Ask in a side chat").press("Enter");
  await expect(side.locator(".message.response")).toContainText(
    "Native worker",
  );
  const merge = side.getByRole("button", { name: "Merge", exact: true });
  await expect(merge).toBeEnabled();
  await merge.click();
  await expect(side).toHaveCount(0);
  // It arrives as a report: named, as the summary alone.
  await expect(transcript.locator(".message-event")).toContainText(
    "Side chat “Btw: Which table?” merged",
  );
  await expect(transcript).toContainText(
    "SIDE-SUMMARY: the table has two columns.",
  );
  await expect(transcript).toContainText("MERGE-HEARD");
  await expect(transcript).not.toContainText("Which table?”.");
  await expect(transcript).not.toContainText("The person merges");
});

test("quotes stack over the draft, stay with it and go before its text", async ({
  page,
  session,
  host,
}) => {
  const other = await savedConversation(host, page.request, "Other chat");
  const transcript = await answered(page, session, {
    width: 1440,
    height: 900,
  });
  const composer = page.getByLabel("Message or guidance");
  const bars = page.locator(".composer .quote-bar");
  const quote = async (text, within = transcript) => {
    await select(within, text);
    await page
      .getByRole("group", { name: "Selection" })
      .getByRole("button", { name: "Quote" })
      .click();
  };
  await quote("answer with a table");
  await expect(bars).toHaveText(["answer with a table"]);
  await expect(composer).toHaveValue("");
  await expect(composer).toBeFocused();
  await expect(page.getByRole("group", { name: "Selection" })).toHaveCount(0);
  await expect(
    page.getByRole("complementary", { name: "Side chats" }),
  ).toHaveCount(0);
  await quote("streamed");
  await expect(bars).toHaveText(["answer with a table", "streamed"]);
  await bars.first().getByRole("button", { name: "Remove the quote" }).click();
  await expect(bars).toHaveText(["streamed"]);
  await quote("answer with a table");
  await expect(bars).toHaveText(["streamed", "answer with a table"]);

  // Another conversation has a draft of its own; this one keeps its quotes.
  await page.getByRole("button", { name: /Other chat/ }).click();
  await expect(page).toHaveURL(new RegExp(other.id));
  await expect(bars).toHaveCount(0);
  await page.getByRole("button", { name: /Hello/ }).first().click();
  await expect(bars).toHaveText(["streamed", "answer with a table"]);

  const sent = (text) =>
    page.waitForRequest(
      (request) =>
        request.url().endsWith("/api/command") &&
        request.postDataJSON().text === text,
    );
  const asked = sent("> streamed\n\n> answer with a table\n\nWhich of them?");
  await composer.fill("Which of them?");
  await composer.press("Enter");
  await asked;
  await expect(bars).toHaveCount(0);
  await expect(composer).toHaveValue("");
  const mine = transcript.locator(".message.user").last();
  await expect(mine.locator("blockquote")).toHaveText([
    "streamed",
    "answer with a table",
  ]);
  await expect(mine).toContainText("Which of them?");

  // A quote alone is a message.
  await expect(
    page.getByRole("button", { name: "Send", exact: true }),
  ).toBeVisible();
  await quote("answer with a table");
  const alone = sent("> answer with a table");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await alone;
  await expect(bars).toHaveCount(0);

  // A command is what was typed: it runs, and the quote waits for the next
  // message.
  await expect(
    page.getByRole("button", { name: "Send", exact: true }),
  ).toBeVisible();
  // The last answer: the first has scrolled away.
  await quote("Native worker", transcript.locator(".message.response").last());
  await composer.fill("/btw Which table?");
  await composer.press("Enter");
  const side = page.getByRole("complementary", { name: "Side chats" });
  await expect(side.getByRole("tab")).toHaveText(["Which table?"]);
  await expect(side.locator(".message.user blockquote")).toHaveCount(0);
  await expect(composer).toHaveValue("");
  await expect(bars).toHaveText(["Native worker"]);
});

test.describe("phone", () => {
  test.use({ isMobile: true, hasTouch: true });

  test("a quote is one line over the input, with a full-size X", async ({
    page,
    session,
  }) => {
    const transcript = await answered(page, session, {
      width: 390,
      height: 844,
    });
    const answer = transcript.locator(".message.response").last();
    await answer.evaluate((node) => {
      const range = document.createRange();
      range.selectNodeContents(node);
      getSelection().removeAllRanges();
      getSelection().addRange(range);
    });
    await page
      .getByRole("group", { name: "Selection" })
      .getByRole("button", { name: "Quote" })
      .click();
    const bar = page.locator(".composer .quote-bar");
    await expect(bar).toHaveCount(1);
    expect(await fits(page)).toBe(true);
    const box = await bar.boundingBox();
    const form = await page.locator(".composer .message-box").boundingBox();
    expect(box.x + box.width).toBeLessThanOrEqual(form.x + form.width);
    const remove = bar.getByRole("button", { name: "Remove the quote" });
    expect((await remove.boundingBox()).height).toBeGreaterThanOrEqual(44);
    await remove.tap();
    await expect(bar).toHaveCount(0);
  });
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
  [1100, "the input asks"],
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
    const [chat, files] = await Promise.all(
      [page.locator(".side-chat"), pane].map(box),
    );
    if (width === 1440) {
      await expect(side.locator(".side-quote")).toContainText("src/app.ts:1-2");
      await expect(side.locator(".side-quote")).toContainText("// One.");
      await expect(side.getByLabel("Ask in a side chat")).toBeFocused();
      const chats = await box(side);
      expect(chats.left).toBeGreaterThanOrEqual(chat.right);
      expect(files.left).toBeGreaterThanOrEqual(chats.right - 1);
      expect(chat.width).toBeGreaterThanOrEqual(360);
    } else {
      // No room for a third column: the conversation's input asks.
      await expect(side).toHaveCount(0);
      const asking = page.locator(".composer .quote-bar");
      await expect(asking).toContainText("Side chat");
      await expect(asking).toContainText("src/app.ts:1-2");
      await expect(page.getByLabel("Message or guidance")).toBeFocused();
      expect(files.left).toBeGreaterThanOrEqual(chat.right - 1);
    }
    expect(await fits(page)).toBe(true);
  });
}

test("on a phone the actions are a bar above the input, which asks; the side chat is its own page", async ({
  page,
  session,
}) => {
  const transcript = await answered(page, session, { width: 390, height: 844 });
  await select(transcript, "answer with a table");
  const actions = page.getByRole("group", { name: "Selection" });
  const input = page.locator(".composer > form");
  const bar = await box(actions);
  expect(bar.bottom).toBeLessThanOrEqual((await box(input)).top);
  await expect(actions).toContainText("answer with a table");
  for (const button of await actions.getByRole("button").all())
    expect((await box(button)).height).toBeGreaterThanOrEqual(44);
  await actions.getByRole("button", { name: "Ask in side chat" }).click();
  // One input: it carries the quote and says where the question goes.
  const prompt = page.getByLabel("Message or guidance");
  const asking = page.locator(".composer .quote-bar");
  await expect(asking).toContainText("answer with a table");
  await expect(prompt).toBeFocused();
  await expect(prompt).toHaveAttribute("placeholder", "Ask about it…");
  await expect(page.getByRole("complementary")).toHaveCount(0);
  // Taking it back leaves an ordinary message to write.
  await asking
    .getByRole("button", { name: "Ask in the conversation instead" })
    .click();
  await expect(asking).toHaveCount(0);
  await select(transcript, "answer with a table");
  await actions.getByRole("button", { name: "Ask in side chat" }).click();
  await prompt.fill("Which table?");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  // Its page: the quote and the question, the answer, Merge and the way back.
  await expect(page).not.toHaveURL(new RegExp(`session=${session.id}$`));
  const asked = transcript.locator(".message.user").last();
  await expect(asked.locator("blockquote")).toHaveText("answer with a table");
  await expect(asked).toContainText("Which table?");
  await expect(asking).toHaveCount(0);
  const head = page.locator(".conversation-head");
  await expect(
    head.getByRole("button", { name: "Merge", exact: true }),
  ).toBeVisible();
  await head.getByRole("button", { name: /^‹/ }).click();
  // Back there, the Activity list holds it.
  const activity = page.locator(".composer .activity-button");
  await expect(activity).toContainText("1 side chat");
  await activity.click();
  await expect(
    page.getByRole("dialog", { name: "Activity" }).locator(".activity-row"),
  ).toHaveText([/Which table\?/]);
  expect(await fits(page)).toBe(true);
});

test("on a phone a long quote is cut short in the message box, which keeps its width", async ({
  page,
  host,
  request,
}) => {
  const line = "A sentence far too long for a phone to show on one line. ";
  const { id } = await savedConversation(host, request, "Long", line.repeat(4));
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${id}`);
  await online(page);
  const transcript = page.locator(".transcript");
  const actions = page.getByRole("group", { name: "Selection" });
  const form = page.locator(".composer > form");
  // The quote and the button that sends stay inside the box's edges.
  const held = async () => {
    const edge = await box(form);
    for (const part of [
      form.locator(".quote-bar"),
      form.getByRole("button", { name: "Send", exact: true }),
    ]) {
      const inner = await box(part);
      expect(inner.left).toBeGreaterThanOrEqual(edge.left);
      expect(inner.right).toBeLessThanOrEqual(edge.right);
    }
  };
  // The answer itself, not the sample drawn while the conversation loads.
  const passage = transcript.locator(".message.response");
  await expect(passage).toContainText(line.repeat(4).trim());
  await select(passage, line.repeat(4).trim());
  await actions.getByRole("button", { name: "Quote" }).click();
  await expect(form.locator(".quote-bar")).toContainText("A sentence far");
  await held();
  // Quote focuses the input a frame later, which takes the selection.
  await expect(page.getByLabel("Message or guidance")).toBeFocused();
  await select(passage, line.repeat(4).trim());
  await actions.getByRole("button", { name: "Ask in side chat" }).click();
  await expect(form.locator(".quote-bar .chip")).toHaveText("Side chat");
  await held();
  expect(await fits(page)).toBe(true);
});

test("on a phone Jump to latest stays clear of the selection's actions", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.getByLabel("Message or guidance").fill("Long continuity probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  const transcript = page.locator(".transcript");
  await expect(transcript).toContainText("late reference");
  await leaveEnd(transcript);
  const jump = page.getByRole("button", { name: /^Jump to latest/ });
  await expect(jump).toBeVisible();
  await select(transcript, "Long retained body sentence.");
  const actions = page.getByRole("group", { name: "Selection" });
  await expect(actions).toBeVisible();
  const [above, bar] = await Promise.all([jump, actions].map(box));
  expect(above.bottom).toBeLessThanOrEqual(bar.top);
});
