// A place's panel opens from the place's name; its board lists who is on it
// and what it started beside the chat, and slides in from the right on a
// phone. What it started lists under the place's name in the sidebar, and
// its members show as avatars on it, each with a card.
import { test, expect, openPanel, headerAction } from "./fixtures.js";

const VIEWPORTS = [
  ["desktop", { width: 1440, height: 900 }],
  ["laptop", { width: 900, height: 860 }],
  ["phone", { width: 390, height: 844 }],
];

test("on a phone the board slides in from the right", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await openPanel(page, session);
  // Nothing stacks above the chat.
  await expect(page.locator(".conversation .board")).toHaveCount(0);
  await page.getByRole("button", { name: /^Board/ }).click();
  const sheet = page.getByRole("dialog", { name: "Board" });
  await expect(
    sheet.getByRole("complementary", { name: "Board" }),
  ).toBeVisible();
  // Settled against the right edge, leaving the chat visible to its left.
  await expect
    .poll(async () => {
      const box = await sheet.boundingBox();
      return Math.round(box.x + box.width);
    })
    .toBe(390);
  expect((await sheet.boundingBox()).x).toBeGreaterThan(30);
  await page.keyboard.press("Escape");
  await expect(sheet).toHaveCount(0);
});

async function shot(page, name) {
  if (process.env.UAGENT_SCREENSHOTS) {
    await page.screenshot({
      path: `${process.env.UAGENT_SCREENSHOTS}/${name}.png`,
    });
  }
}

for (const [name, viewport] of VIEWPORTS) {
  test(`panel view and help (${name})`, async ({
    page,
    session,
    browserName,
  }) => {
    await page.setViewportSize(viewport);
    await openPanel(page, session);
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    // One stable geometry: nothing scrolls the page sideways.
    expect(
      await page.evaluate(
        () => document.documentElement.scrollWidth - window.innerWidth,
      ),
    ).toBeLessThanOrEqual(0);
    const head = page.locator(".conversation-head");
    if (name === "phone") {
      // A phone keeps the title, the board and the menu, which holds the
      // instructions.
      await expect(head.locator(".panel-subtitle")).toHaveCount(0);
      await head.getByRole("button", { name: "Conversation menu" }).click();
      await expect(
        page.getByRole("menuitem", { name: "Edit instructions" }),
      ).toBeVisible();
      await page.keyboard.press("Escape");
    } else {
      // What it is, in one line under its title, with its instructions a
      // tap away even where the words truncate.
      const subtitle = page.locator(".panel-subtitle");
      await expect(subtitle).toContainText("hands work to threads");
      const button = subtitle.getByRole("button", {
        name: "Edit instructions",
      });
      await expect(button).toBeInViewport();
      expect((await subtitle.boundingBox()).height).toBeLessThan(40);
    }
    // Nothing the title column holds reaches under the buttons beside it.
    expect(
      await head.evaluate((node) => {
        const title = node.querySelector(":scope > div");
        const next = title.nextElementSibling;
        if (!next) return true;
        const edge = next.getBoundingClientRect().left;
        return [...title.querySelectorAll("*")].every(
          (child) => child.getBoundingClientRect().right <= edge + 0.5,
        );
      }),
    ).toBe(true);
    await shot(page, `help-${browserName}-${name}`);
  });

  test(`panel turns keep their order (${name})`, async ({
    page,
    session,
    command,
    request,
    browserName,
  }) => {
    await page.setViewportSize(viewport);
    const id = await openPanel(page, session);
    let meta;
    await expect
      .poll(async () => {
        meta = (await (await request.get(`/api/sessions/${id}`)).json())
          .metadata;
        return meta.generation;
      })
      .toBeTruthy();
    await command("model", {
      session_id: id,
      generation: meta.generation,
      operation: "select",
      model: "mock/model-b",
    });
    const prompt = page.getByLabel("Message or guidance");
    for (const [index, text] of ["first", "second", "third"].entries()) {
      await prompt.fill(text);
      await prompt.press("Enter");
      await expect(page.locator(".turn-summary")).toHaveCount(index + 1);
    }
    // Each answer keeps its own stats line, in order, and what was typed is
    // shown as typed.
    const rows = await page
      .locator(".transcript-content > *")
      .evaluateAll((nodes) =>
        nodes.map((node) =>
          node.classList.contains("turn-footer")
            ? "stats"
            : node.classList.contains("user")
              ? // What shows: the row's spoken name ("You, 10:32") is not.
                [...node.children]
                  .filter((child) => !child.classList.contains("sr-only"))
                  .map((child) => child.textContent)
                  .join("")
                  .replace(/^\s*\d+:\d+\s*(AM|PM)?/, "")
                  .trim()
              : "",
        ),
      );
    expect(rows.filter(Boolean)).toEqual([
      "first",
      "stats",
      "second",
      "stats",
      "third",
      "stats",
    ]);
    // At the bottom, there is nothing to jump to.
    await expect(page.locator(".jump")).toHaveCount(0);
    await shot(page, `turns-${browserName}-${name}`);
  });
}

for (const [name, viewport] of VIEWPORTS) {
  test(`decisions waiting on you show above the composer (${name})`, async ({
    page,
    session,
    command,
    browserName,
  }) => {
    await page.setViewportSize(viewport);
    // A conversation in the folder asks to save a memory, in Ask mode.
    await command("model", {
      session_id: session.id,
      generation: session.generation,
      operation: "select",
      model: "mock/model-b",
    });
    await command("submit", {
      session_id: session.id,
      generation: session.generation,
      text: "Memory receipt probe: please save this test memory",
    });
    await openPanel(page, session);
    // The panel is the place's name now, and what waits is said once in
    // the list: by the inbox above it.
    if (name === "desktop") {
      await expect(page.getByTitle(/^Panel of /)).toHaveAttribute(
        "aria-current",
        "page",
      );
    }
    const waiting = page.getByRole("region", {
      name: "Decisions waiting on you",
    });
    await expect(waiting.getByText("Needs your decision")).toBeVisible();
    await expect(waiting.getByText("Allow memory?")).toBeVisible();
    // The transcript keeps at least half the column.
    const box = await waiting.boundingBox();
    const column = await page.locator("#conversation").boundingBox();
    expect(box.height).toBeLessThanOrEqual(column.height / 2 + 1);
    await shot(page, `escalation-${browserName}-${name}`);
    await waiting.getByRole("button", { name: "Allow once" }).click();
    await expect(waiting).toHaveCount(0);
  });
}

test("a member joins the chat, is addressed by name and answers under its name", async ({
  page,
  session,
  command,
  request,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  const id = await openPanel(page, session);
  let meta;
  await expect
    .poll(async () => {
      meta = (await (await request.get(`/api/sessions/${id}`)).json()).metadata;
      return meta.generation;
    })
    .toBeTruthy();
  await command("model", {
    session_id: id,
    generation: meta.generation,
    operation: "select",
    model: "mock/model-b",
  });
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Chat probe");
  await prompt.press("Enter");
  // The member is listed on the board with its avatar, and in the sidebar
  // by its avatar on the place's name, after the moderator's; not as a row.
  // Its introduction is a message under its name, without the name the
  // model reads it by. The panel is named above its own.
  const board = page.getByRole("complementary", { name: "Board" });
  await expect(board.getByRole("heading", { name: /^Members/ })).toBeVisible();
  await expect(board.locator(".avatar")).toHaveText("A");
  const seats = page.locator(".sidebar .place-seats .avatar");
  await expect(seats).toHaveText(["M", "A"]);
  await expect(page.locator(".sidebar .session .avatar")).toHaveCount(0);
  // The avatars have a line of their own below the place's name, which
  // keeps the header's width.
  const name = await page
    .getByTitle(/^Panel of /)
    .first()
    .boundingBox();
  const seat = await seats.first().boundingBox();
  expect(seat.y).toBeGreaterThanOrEqual(name.y + name.height);
  await page.reload();
  await expect(seats).toHaveText(["M", "A"]);
  const said = page.locator(".transcript .message", {
    has: page.locator(".actor", { hasText: "Ada" }),
  });
  await expect(said.first()).toContainText("Ada here.");
  await expect(said.first()).not.toContainText("Ada:");
  await expect(said.first().locator(".avatar")).toHaveText("A");
  await expect(
    page.locator(".transcript .message .actor", { hasText: "Moderator" }),
  ).toHaveCount(1);
  await expect(page.locator(".turn-summary")).toHaveCount(1);

  // A member is addressed by name, never with an @, which offers none.
  await prompt.pressSequentially("@A");
  await expect(page.getByRole("option")).toHaveCount(0);
  // From here on, whatever is added to the transcript is recorded, and so is
  // how tall it is and what the state above the input says: a pass must add
  // nothing and move nothing, and the state names who is typing and never
  // what anyone is thinking.
  await page.evaluate(() => {
    window.added = [];
    window.states = new Set();
    const content = document.querySelector(".transcript-content");
    new MutationObserver((records) => {
      for (const record of records)
        for (const node of record.addedNodes)
          window.added.push(node.textContent || "");
    }).observe(content, {
      childList: true,
      subtree: true,
      characterData: true,
    });
    const status = document.querySelector(".composer .activity-status");
    new MutationObserver(() =>
      window.states.add(status.querySelector(".activity-caption").textContent),
    ).observe(status, { childList: true, subtree: true, characterData: true });
  });
  // A message that opens with its name wakes it alone, and its answer goes
  // back to who asked: the panel reads both without a turn.
  const status = page.locator(".composer .activity-caption");
  await prompt.fill("Ada, Second opinion");
  await prompt.press("Enter");
  await expect(status).toHaveText("Ada is typing…");
  await expect(said.last()).toContainText("I would ship it.");
  await expect(status).toHaveText("Ready");
  // One for everyone wakes both. The panel passes, slowly, and passes
  // again on what Ada answers; a pass is shown to nobody: no bubble, no
  // stats line.
  await prompt.fill("Second opinion, anyone?");
  await prompt.press("Enter");
  await expect(status).toHaveText("Ada and Moderator are typing…");
  await expect(said).toHaveCount(3);
  const height = () =>
    page.evaluate(
      () => document.querySelector(".transcript-content").scrollHeight,
    );
  const answered = await height();
  await expect
    .poll(async () => {
      const blocks = (await (await request.get(`/api/sessions/${id}`)).json())
        .state.view.blocks;
      return blocks.filter((block) => block.silent).length;
    })
    .toBeGreaterThan(0);
  await expect(page.locator(".composer .status-led.running")).toHaveCount(0);
  await expect(status).toHaveText("Ready");
  expect(
    [...(await page.evaluate(() => [...window.states]))].filter(
      (text) =>
        !/^(Ready|(Ada|Moderator|Ada and Moderator) (is|are) typing…)$/.test(
          text,
        ),
    ),
  ).toEqual([]);
  await expect(page.locator(".transcript")).not.toContainText("PASS");
  // Nothing of the panel's turn was ever in the transcript, and from
  // Ada's answer on its height never changed.
  const added = await page.evaluate(() => window.added);
  expect(added.filter((text) => /PA|Nothing to add/.test(text))).toEqual([]);
  expect(await height()).toBe(answered);
  await expect(page.locator(".turn-summary")).toHaveCount(1);
  await expect(
    page.locator(".transcript .message .actor", { hasText: "Moderator" }),
  ).toHaveCount(1);

  // What a member was sent is one click from what it wrote.
  await said.last().hover();
  await said
    .last()
    .getByRole("button", { name: /^Actions for Ada/ })
    .click();
  await page.getByRole("menuitem", { name: "Show prompt" }).click();
  await expect(page.getByRole("dialog")).toContainText(
    "You are Ada, a member of the panel",
  );
  await page.keyboard.press("Escape");
  await shot(page, "panel-chat");

  // A long message of a member's shows its opening, and the rest on request.
  await prompt.fill("Ada, Long opinion");
  await prompt.press("Enter");
  await expect(said.last()).toContainText("Long. Long.");
  await expect(said.last()).not.toContainText("LONG-END");
  await said.last().getByRole("button", { name: "Show full message" }).click();
  await expect(said.last()).toContainText("LONG-END");
  await expect(said.last()).not.toContainText("Ada:");

  // Renamed, a member is called that everywhere: above what it wrote before,
  // in the list, and by a message that opens with either name.
  const ada = (await (await request.get("/api/sessions")).json()).sessions.find(
    (item) => item.member === "Ada",
  );
  await command("rename", {
    session_id: ada.id,
    generation: ada.generation,
    title: "Ada Lovelace",
  });
  const renamed = page.locator(".transcript .message", {
    has: page.locator(".actor", { hasText: "Ada Lovelace" }),
  });
  await expect(renamed.first()).toContainText("Ada here.");
  await expect(renamed.first()).not.toContainText("Ada:");
  await expect(board.locator(".avatar")).toHaveText("AL");
  await expect(board).toContainText("Ada Lovelace");
  const posts = await renamed.count();
  await prompt.fill("Ada Lovelace, Second opinion");
  await prompt.press("Enter");
  await expect(renamed).toHaveCount(posts + 1);
  await prompt.fill("Ada, Second opinion");
  await prompt.press("Enter");
  await expect(renamed).toHaveCount(posts + 2);
  await expect(renamed.last()).not.toContainText("Lovelace:");
});

test("the panel's subtitle opens its instructions", async ({
  page,
  session,
}) => {
  await openPanel(page, session);
  await headerAction(page, "Edit instructions");
  const dialog = page.getByRole("dialog", { name: "Instructions" });
  const panel = dialog.getByLabel("Yours · panel");
  await expect(panel).toBeEnabled();
  await panel.fill("Prefer small threads.");
  await dialog.getByRole("button", { name: "Save", exact: true }).click();
  await expect(dialog.getByRole("button", { name: "Save" })).toHaveCount(0);
  // Saving keeps focus in the field, not on the page behind the dialog.
  await expect(panel).toBeFocused();
  await shot(page, "instructions");
});

test("instructions read well on a phone", async ({ page, session }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await openPanel(page, session);
  await headerAction(page, "Edit instructions");
  const dialog = page.getByRole("dialog", { name: "Instructions" });
  await expect(dialog.getByLabel("Yours · panel")).toBeEnabled();
  expect(
    await dialog.evaluate((node) => node.scrollWidth <= node.clientWidth + 1),
  ).toBe(true);
  await shot(page, "instructions-phone");
});
