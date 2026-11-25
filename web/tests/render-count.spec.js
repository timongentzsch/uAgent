import {
  devServerTest as test,
  expect,
  DEV_SERVER_URL,
  fakeStream,
} from "./fixtures.js";

// The dev page on a mocked host; src/render-count.ts counts renders, so a
// re-render of the shell or of every row per streamed frame fails here.
const EPOCH = "render-epoch";
const ID = "a".repeat(32);
// Changed within the last day, so Recent lists every row open.
const NOW = Date.now();
const session = (id, index) => ({
  id,
  generation: `g-${id}`,
  title: `Conversation ${index}`,
  cwd: `/project-${index % 4}`,
  status: "idle",
  presence: "active",
  updated: NOW - index * 60_000,
});
// A working agent, whose rows the stream must leave alone.
const PARENT = { ...session("c".repeat(32), 90), cwd: "/project-1" };
const AGENT = {
  ...session("d".repeat(32), 91),
  cwd: "/project-1",
  parent: PARENT.id,
  turn_active: true,
};
const SESSIONS = [
  session(ID, 0),
  PARENT,
  AGENT,
  // Zero-padded, so no two ids are the same.
  ...Array.from({ length: 1000 }, (_, index) =>
    session((index + 1).toString(16).padStart(32, "0"), index + 1),
  ),
];
const HISTORY = Array.from({ length: 120 }, (_, index) => ({
  id: `m-${index}`,
  parent: index % 2 ? `m-${index - 1}` : null,
  kind: index % 2 ? "message" : "turn",
  ...(index % 2 && { role: "assistant" }),
  body: { kind: "text", text: `Message ${index} with a sentence of history.` },
  sequence: index + 1,
}));

// One existing child reply stays still while the next answer streams.
HISTORY[1] = {
  ...HISTORY[1],
  role: "user",
  origin: "agent",
  agent: { session: "child", title: "Earlier research", outcome: "completed" },
};

// In the page, testSend sends the host's next event; testSince counts the
// renders and storage writes (by key) since testMark.
async function open(page) {
  let reads = 0;
  await page.route("**/sw.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "" }),
  );
  await page.route("**/api/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    const json = (body, status = 200) =>
      route.fulfill({ status, json: { v: 2, ...body } });
    if (path === "/api/sessions")
      return json({
        epoch: EPOCH,
        cursor: HISTORY.length,
        sessions: SESSIONS,
        capabilities: {},
        devices: [],
        commands: [],
      });
    if (path === "/api/search")
      return json({
        hits: SESSIONS.map(({ id, title }) => ({
          session_id: id,
          title,
          sequence: 1,
          snippet: "the word",
          rows: 1,
        })),
      });
    // Every conversation reads as the same history; its folder lists one file.
    const [, id, files] =
      /^\/api\/sessions\/(\w{32})(\/files)?$/.exec(path) || [];
    const metadata = SESSIONS.find((item) => item.id === id);
    if (
      metadata &&
      files &&
      new URL(route.request().url()).searchParams.has("read")
    )
      return json({
        text: "# Project notes",
        bytes: 15,
        more: false,
        revision: "1",
      });
    if (metadata && files)
      return json({
        entries: [{ name: "notes.md", dir: false, size: 1 }],
        changes: [],
        more: false,
        repository: true,
        stamps: {},
      });
    // Others are busy meanwhile: each later read of another conversation
    // answers at a later cursor.
    if (metadata)
      return json({
        epoch: EPOCH,
        cursor: HISTORY.length + (id === ID ? 0 : ++reads),
        metadata,
        state: { phase: "idle", view: { nodes: HISTORY } },
        pending: null,
      });
    return json({ error: "Not in this mock" }, 404);
  });
  await page.addInitScript(fakeStream, {
    epoch: EPOCH,
    cursor: HISTORY.length,
  });
  await page.addInitScript(
    ({ epoch, cursor, id, generation }) => {
      // The level these counts were taken at: every step a row.
      localStorage.setItem("uagent-detail", "default");
      globalThis.testSequence = cursor + 1;
      globalThis.testSend = (fields) =>
        globalThis.testStream.dispatchEvent(
          new MessageEvent("update", {
            data: JSON.stringify({
              v: 2,
              epoch,
              sequence: globalThis.testSequence++,
              session_id: id,
              generation,
              ...fields,
            }),
          }),
        );
      globalThis.testFrame = () =>
        new Promise((resolve) => setTimeout(resolve, 20));
      const writes = {};
      const setItem = Storage.prototype.setItem;
      Storage.prototype.setItem = function (key, value) {
        writes[key] = (writes[key] || 0) + 1;
        return setItem.call(this, key, value);
      };
      // Late lists and settings still render after the first paint: counting
      // starts once nothing has rendered for half a second.
      let marked;
      globalThis.testQuiet = async () => {
        for (let still = 0, seen = ""; still < 25; await testFrame()) {
          const now = JSON.stringify(globalThis.renderCounts);
          still = now === seen ? still + 1 : 0;
          seen = now;
        }
      };
      globalThis.testMark = async () => {
        await testQuiet();
        marked = {
          renders: { ...globalThis.renderCounts },
          writes: { ...writes },
        };
      };
      const since = (now, before) =>
        Object.fromEntries(
          Object.entries(now)
            .map(([name, count]) => [name, count - (before[name] || 0)])
            .filter(([, count]) => count),
        );
      globalThis.testSince = () => ({
        renders: since(globalThis.renderCounts, marked.renders),
        writes: since(writes, marked.writes),
      });
    },
    {
      epoch: EPOCH,
      cursor: HISTORY.length,
      id: ID,
      generation: SESSIONS[0].generation,
    },
  );
  await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
  await expect(page.getByLabel("Message or guidance")).toBeEnabled();
  await expect(page.locator('[data-message-id="m-119"]')).toBeVisible();
  await page.evaluate(() => import("/src/render-count.ts"));
}

// What a test counted, in its report: the numbers a budget is set from.
const measured = (type, counts) =>
  test.info().annotations.push({ type, description: JSON.stringify(counts) });

const streamReply = (page) =>
  page.evaluate(async () => {
    testSend({
      kind: "node",
      node: {
        id: "w-m-118",
        parent: "m-118",
        kind: "work",
        state: "running",
        headline: { verb: "", target: "0 steps" },
      },
    });
    testSend({
      kind: "node",
      node: {
        id: "reply",
        parent: "m-118",
        kind: "message",
        role: "assistant",
        streaming: true,
        body: { kind: "markdown", text: "Reply: " },
        sequence: testSequence,
      },
    });
    for (let index = 0; index < 60; index++) {
      testSend({
        kind: "node",
        id: "reply",
        append: { text: `word${index} ` },
      });
      if (index % 10 === 9)
        for (const state of ["running", "ok"])
          testSend({
            kind: "node",
            node: {
              id: `tool-${index}`,
              parent: "w-m-118",
              kind: "call",
              headline: {
                verb: state === "ok" ? "Read" : "Reading",
                target: `file-${index}.ts`,
              },
              state,
              ...(state === "ok" && { body: { kind: "text", text: "read" } }),
              sequence: testSequence,
            },
          });
      await testFrame();
    }
    testSend({ kind: "node", id: "reply", set: { streaming: false } });
    await testFrame();
  });

test("streaming renders only what changed", async ({ page }) => {
  await open(page);
  await page.evaluate(() => testMark());
  await streamReply(page);
  const { renders: counts } = await page.evaluate(() => testSince());
  await expect(page.locator('[data-message-id="reply"]')).toContainText(
    "word59",
  );
  measured("streamed", counts);
  // Each frame renders the conversation once and only the node it touched.
  expect(counts.App || 0).toBe(0);
  expect(counts.SidebarView || 0).toBe(0);
  expect(counts.SessionRow || 0).toBe(0);
  expect(counts.ChatPage).toBeLessThanOrEqual(62);
  expect(counts.MessageView).toBeLessThanOrEqual(62);
  expect(counts.RowLine).toBeLessThanOrEqual(6 * 2 + 1);
  for (const name of [
    "Composer",
    "ModelControl",
    "ActivityButtonView",
    "AgentSender",
  ])
    expect(counts[name] || 0, name).toBe(0);

  await page.evaluate(async () => {
    testSend({
      kind: "node",
      node: {
        id: "spawn",
        parent: "w-m-118",
        kind: "call",
        headline: { verb: "Started", target: "agent" },
        state: "ok",
        body: { kind: "text", text: "started" },
        parts: [
          { kind: "link", to: "agent", id: "child", label: "Inspect tests" },
        ],
        sequence: testSequence,
      },
    });
    await testMark();
    for (let index = 0; index < 20; index++) {
      testSend({
        kind: "event",
        type: "activities.changed",
        data: {
          activities: [
            {
              id: 1,
              label: "npm test",
              status: "running",
              progress: `step ${index}`,
            },
          ],
        },
      });
      await testFrame();
    }
  });
  const { renders: ticks } = await page.evaluate(() => testSince());
  await expect(
    page.locator('[data-message-id="spawn"]').locator("> .node-row"),
  ).toContainText("Started agent");
  expect(ticks.RowLine || 0).toBe(0);
  expect(ticks.MessageView || 0).toBe(0);
  expect(ticks.SessionRow).toBeGreaterThan(0);
  expect(ticks.SessionRow).toBeLessThanOrEqual(20);

  // Every conversation holds "word": more hits than the panel shows at once.
  await page.keyboard.press("ControlOrMeta+k");
  const palette = page.getByRole("dialog", { name: "Search" });
  await palette.getByRole("combobox").fill("word");
  const results = palette.getByRole("listbox", { name: "Results" });
  await expect(results.locator(".spotlight-snippet").first()).toBeVisible();
  const bottom = await palette.evaluate(
    (dialog) => dialog.getBoundingClientRect().bottom,
  );
  expect(bottom).toBeLessThanOrEqual(page.viewportSize().height);
  await results.evaluate((list) => (list.scrollTop = list.scrollHeight));
  const scrolled = await results.evaluate((list) => list.scrollTop);
  expect(scrolled).toBeGreaterThan(0);
  const renders = await page.evaluate(async () => {
    const before = globalThis.renderCounts.Spotlight || 0;
    for (let index = 0; index < 10; index++) {
      testSend({
        kind: "event",
        type: "activities.changed",
        data: {
          activities: [
            {
              id: 1,
              label: "npm test",
              status: "running",
              progress: `later ${index}`,
            },
          ],
        },
      });
      await testFrame();
    }
    return (globalThis.renderCounts.Spotlight || 0) - before;
  });
  // The premise: search did re-render under the person's scroll.
  expect(renders).toBeGreaterThan(0);
  expect(await results.evaluate((list) => list.scrollTop)).toBe(scrolled);
});

for (const phone of [false, true])
  test(`typing only renders the composer on ${phone ? "a phone" : "desktop beside Files"}`, async ({
    page,
  }) => {
    await page.setViewportSize(
      phone ? { width: 390, height: 844 } : { width: 1440, height: 900 },
    );
    await open(page);
    if (!phone) {
      await page.getByRole("button", { name: "Files", exact: true }).click();
      const files = page.getByRole("region", { name: "Files" });
      await files.getByRole("button", { name: "notes.md" }).click();
      await expect(files.locator(".markdown")).toContainText("Project notes");
    }
    const prompt = page.getByLabel("Message or guidance");
    await prompt.focus();
    await page.evaluate(() => testMark());
    await prompt.pressSequentially("twenty characters!!!");
    const { renders, writes } = await page.evaluate(() => testSince());
    measured("typed", { renders, writes });
    expect(renders.Composer).toBeGreaterThan(0);
    for (const name of [
      "HostsShell",
      "App",
      "ChatPage",
      "ConversationHeader",
      "Modals",
      "Files",
      "MarkdownFile",
      "SidebarView",
      "Chat",
      "ModelControl",
      "SessionSummaryView",
      "ActivityButtonView",
    ])
      expect(renders[name] || 0, name).toBe(0);
    const key = `uagent-drafts:${ID}`;
    expect(writes[key] || 0).toBe(0);
    await expect
      .poll(() => page.evaluate((key) => testSince().writes[key], key))
      .toBe(1);
  });

test("a background conversation resumes at the latest streamed reply", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await open(page);
  await page.getByRole("button", { name: "Library", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "Library", exact: true }).first(),
  ).toBeVisible();
  await page.evaluate(() => testMark());
  await streamReply(page);
  const { renders } = await page.evaluate(() => testSince());
  measured("background", renders);
  // The first frame updates the catalogue; text frames only update its store.
  expect(renders.ChatPage || 0).toBeLessThanOrEqual(1);
  await page.goBack();
  await expect(page.locator('[data-message-id="reply"]')).toContainText(
    "word59",
  );
});

test("state frames and an open dialog leave the shell alone", async ({
  page,
}) => {
  await open(page);
  const turn = { ...SESSIONS[0], status: "running", turn_active: true };
  const frames = await page.evaluate(async (metadata) => {
    await testMark();
    for (let index = 0; index < 20; index++) {
      testSend({
        kind: "state",
        phase: "working",
        state: { phase: "working", turn_status: "Working" },
        pending: null,
        metadata: { ...metadata, turn_status: "Working" },
      });
      await testFrame();
    }
    const state = testSince().renders;
    await testMark();
    for (let index = 0; index < 20; index++) {
      testSend({
        kind: "turn_status",
        turn_status: `Reading file ${index}`,
        phase: "working",
        metadata: { ...metadata, turn_status: `Reading file ${index}` },
      });
      await testFrame();
    }
    return { state, activity: testSince().renders };
  }, turn);
  measured("frames", frames);
  // The first frame starts the turn; the others restate it.
  for (const name of ["App", "SidebarView", "SessionRow", "Composer"])
    expect(frames.state[name] || 0, name).toBeLessThanOrEqual(1);
  // A new caption renders the shell and that one row once a frame.
  for (const name of ["App", "SidebarView", "SessionRow"])
    expect(frames.activity[name] || 0, name).toBeLessThanOrEqual(20);
  expect(frames.activity.Composer || 0).toBeLessThanOrEqual(40);

  await page.getByRole("button", { name: "Settings" }).click();
  await expect(
    page.getByRole("heading", { name: "Settings", exact: true }),
  ).toBeVisible();
  await page.evaluate(() => testMark());
  await streamReply(page);
  const { renders } = await page.evaluate(() => testSince());
  measured("settings", renders);
  for (const name of ["Modals", "SettingsNav"])
    expect(renders[name] || 0, name).toBe(0);
});

// A conversation in the list and its id, by its title.
const titled = (title) => SESSIONS.find((item) => item.title === title).id;
const listed = (page, title) =>
  page
    .locator(".session-row")
    .filter({ has: page.getByText(title, { exact: true }) })
    .locator("button.session");

// Chooses a conversation in the list and counts, until nothing renders, what
// that drew and wrote, and the host reads it made.
async function choose(page, title) {
  const reads = [];
  const read = (request) => {
    const url = new URL(request.url());
    reads.push(url.pathname + url.search);
  };
  page.on("request", read);
  // The transcript's placeholder rows added on the way.
  await page.evaluate(() => {
    const placeholder = '[data-message-id^="placeholder-"]';
    globalThis.testPlaceholders = 0;
    globalThis.testWatch?.disconnect();
    globalThis.testWatch = new MutationObserver((records) => {
      for (const record of records)
        for (const node of record.addedNodes)
          if (node instanceof Element)
            testPlaceholders +=
              node.querySelectorAll(placeholder).length +
              (node.matches(placeholder) ? 1 : 0);
    });
    testWatch.observe(document.body, { subtree: true, childList: true });
    return testMark();
  });
  await listed(page, title).click();
  await expect(
    page.locator(
      `.transcript[data-session="${titled(title)}"]:not([aria-busy]) [data-message-id="m-119"]`,
    ),
  ).toBeVisible();
  const counted = await page.evaluate(async () => {
    await testQuiet();
    return { ...testSince(), placeholders: testPlaceholders };
  });
  page.off("request", read);
  return { ...counted, reads };
}

// Conversation 5 is in another folder than Conversation 0.
test("a switch draws two rows of the list and each message once", async ({
  page,
}) => {
  await open(page);
  const first = await choose(page, "Conversation 5");
  measured("first visit", first);
  // Not read yet: its placeholder shows until it is.
  expect.soft(first.placeholders).toBeGreaterThan(0);
  // Each message is drawn once as it appears.
  expect
    .soft(first.renders.RenderedMarkdown)
    .toBeLessThanOrEqual(HISTORY.length);
  // Both are held now.
  await choose(page, "Conversation 0");
  const held = await choose(page, "Conversation 5");
  measured("held", held);
  // Held: its messages are drawn in the first frame, no placeholder.
  expect.soft(held.placeholders).toBe(0);
  // Its read answers later than the view held: the rows it restates stay.
  expect.soft(held.renders.MessageView).toBeLessThanOrEqual(HISTORY.length);
  // The rows that lost and gained the selection, not the whole list.
  expect.soft(held.renders.SessionRow).toBeLessThanOrEqual(2);
  // The shell once, for the press: the conversation's read adds nothing.
  expect.soft(held.renders.App).toBe(1);
  expect
    .soft(
      held.reads.filter(
        (path) => path === `/api/sessions/${titled("Conversation 5")}`,
      ),
    )
    .toHaveLength(1);
});

test("Files open over a switch reads and keeps its folder once", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await open(page);
  await page.getByRole("button", { name: "Files", exact: true }).click();
  await expect(page.getByRole("region", { name: "Files" })).toContainText(
    "notes.md",
  );
  const { reads, writes } = await choose(page, "Conversation 5");
  measured("files", { reads, writes });
  expect
    .soft(
      reads.filter(
        (path) =>
          path === `/api/sessions/${titled("Conversation 5")}/files?list=`,
      ),
    )
    .toHaveLength(1);
  expect.soft(writes["uagent-files-at"]).toBe(1);
});
