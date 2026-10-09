// A folder's coordinator opens from its header icon; its board lists the
// folder's sessions beside the chat, and slides in from the right on a
// phone. Its threads nest under that header in the sidebar.
import { test, expect } from "./fixtures.js";

const VIEWPORTS = [
  ["desktop", { width: 1440, height: 900 }],
  ["laptop", { width: 900, height: 860 }],
  ["phone", { width: 390, height: 844 }],
];

async function openCoordinator(page, session) {
  await page.goto(`/#session=${session.id}`);
  // Narrow layouts keep the sessions in a drawer.
  const drawer = page.getByLabel("Open conversations");
  const button = page.getByLabel(/^Coordinator for /).first();
  await expect(drawer.or(button)).toBeVisible();
  if (await drawer.isVisible()) await drawer.click();
  await page
    .getByLabel(/^Coordinator for /)
    .first()
    .click();
  // Beside the chat, or behind the header's Board button on a phone.
  await expect(
    page
      .getByRole("complementary", { name: "Board" })
      .or(page.getByRole("button", { name: /^Board/ })),
  ).toBeVisible();
  return new URL(page.url()).hash.match(/session=([a-f0-9]+)/)[1];
}

test("on a phone the board slides in from the right", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await openCoordinator(page, session);
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
  test(`coordinator view and help (${name})`, async ({
    page,
    session,
    browserName,
  }) => {
    await page.setViewportSize(viewport);
    await openCoordinator(page, session);
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    // One stable geometry: nothing scrolls the page sideways.
    expect(
      await page.evaluate(
        () => document.documentElement.scrollWidth - window.innerWidth,
      ),
    ).toBeLessThanOrEqual(0);
    // What it is, in one line under its title, with its instructions a tap
    // away even where the words truncate.
    const subtitle = page.locator(".coordinator-subtitle");
    await expect(subtitle).toContainText("hands work to threads");
    const button = subtitle.getByRole("button", { name: "Edit instructions" });
    await expect(button).toBeInViewport();
    expect((await subtitle.boundingBox()).height).toBeLessThan(40);
    await shot(page, `help-${browserName}-${name}`);
  });

  test(`coordinator turns keep their order (${name})`, async ({
    page,
    session,
    command,
    request,
    browserName,
  }) => {
    await page.setViewportSize(viewport);
    const id = await openCoordinator(page, session);
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
          node.classList.contains("turn-summary")
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
    await openCoordinator(page, session);
    // The folder's coordinator button names what waits on you.
    if (name === "desktop")
      await expect(
        page.getByLabel(/^Coordinator for .+, 1 waiting on you$/),
      ).toBeVisible();
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
  const id = await openCoordinator(page, session);
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
  // The member is listed on the board and in the sidebar with its avatar,
  // and its introduction is a message under its name, without the name the
  // model reads it by. The coordinator is named above its own.
  const board = page.getByRole("complementary", { name: "Board" });
  await expect(board.getByRole("heading", { name: /^Members/ })).toBeVisible();
  await expect(board.locator(".avatar")).toHaveText("A");
  const said = page.locator(".transcript .message", {
    has: page.locator(".actor", { hasText: "Ada" }),
  });
  await expect(said.first()).toContainText("Ada here.");
  await expect(said.first()).not.toContainText("Ada:");
  await expect(said.first().locator(".avatar")).toHaveText("A");
  await expect(
    page.locator(".transcript .message .actor", { hasText: "Coordinator" }),
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
  // back to who asked: the coordinator reads both without a turn.
  const status = page.locator(".composer .activity-caption");
  await prompt.fill("Ada, Second opinion");
  await prompt.press("Enter");
  await expect(status).toHaveText("Ada is typing…");
  await expect(said.last()).toContainText("I would ship it.");
  await expect(status).toHaveText("Ready");
  // One for everyone wakes both. The coordinator passes, slowly, and passes
  // again on what Ada answers; a pass is shown to nobody: no bubble, no
  // stats line.
  await prompt.fill("Second opinion, anyone?");
  await prompt.press("Enter");
  await expect(status).toHaveText("Ada and Coordinator are typing…");
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
        !/^(Ready|(Ada|Coordinator|Ada and Coordinator) (is|are) typing…)$/.test(
          text,
        ),
    ),
  ).toEqual([]);
  await expect(page.locator(".transcript")).not.toContainText("PASS");
  // Nothing of the coordinator's turn was ever in the transcript, and from
  // Ada's answer on its height never changed.
  const added = await page.evaluate(() => window.added);
  expect(added.filter((text) => /PA|Nothing to add/.test(text))).toEqual([]);
  expect(await height()).toBe(answered);
  await expect(page.locator(".turn-summary")).toHaveCount(1);
  await expect(
    page.locator(".transcript .message .actor", { hasText: "Coordinator" }),
  ).toHaveCount(1);

  // What a member was sent is one click from what it wrote.
  await said.last().hover();
  await said
    .last()
    .getByRole("button", { name: /^Actions for Ada/ })
    .click();
  await page.getByRole("menuitem", { name: "Show prompt" }).click();
  await expect(page.getByRole("dialog")).toContainText(
    "You are Ada, a member of a chat",
  );
  await page.keyboard.press("Escape");
  await shot(page, "coordinator-chat");
});

test("the coordinator's subtitle opens its instructions", async ({
  page,
  session,
}) => {
  await openCoordinator(page, session);
  await page.getByRole("button", { name: "Edit instructions" }).click();
  const dialog = page.getByRole("dialog", { name: "Instructions" });
  const coordinator = dialog.getByLabel("Yours · coordinator");
  await expect(coordinator).toBeEnabled();
  await coordinator.fill("Prefer small threads.");
  await dialog.getByRole("button", { name: "Save", exact: true }).click();
  await expect(dialog.getByRole("button", { name: "Save" })).toHaveCount(0);
  // Saving keeps focus in the field, not on the page behind the dialog.
  await expect(coordinator).toBeFocused();
  await shot(page, "instructions");
});

test("instructions read well on a phone", async ({ page, session }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await openCoordinator(page, session);
  await page.getByRole("button", { name: "Edit instructions" }).click();
  const dialog = page.getByRole("dialog", { name: "Instructions" });
  await expect(dialog.getByLabel("Yours · coordinator")).toBeEnabled();
  expect(
    await dialog.evaluate((node) => node.scrollWidth <= node.clientWidth + 1),
  ).toBe(true);
  await shot(page, "instructions-phone");
});
