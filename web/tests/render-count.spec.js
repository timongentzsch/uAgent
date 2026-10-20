import { showcaseTest as test, expect, SHOWCASE_URL } from "./fixtures.js";

// The Vite-served app (a development page, like the showcase) on a mocked
// host: a saved conversation, a long sidebar and a streamed reply. Counts
// renders with the dev-only src/render-count.ts, so a regression that
// re-renders the shell or every row per streamed frame fails here.
const EPOCH = "render-epoch";
const ID = "a".repeat(32);
const session = (id, index) => ({
  id,
  generation: `g-${id}`,
  title: `Conversation ${index}`,
  cwd: `/project-${index % 4}`,
  status: "idle",
  presence: "active",
  updated: 1_700_000_000_000 - index * 60_000,
});
// A panel with a thread under it, whose rows the stream must leave alone.
const PANEL = {
  ...session("c".repeat(32), 90),
  kind: "panel",
  cwd: "/project-1",
};
const THREAD = {
  ...session("d".repeat(32), 91),
  kind: "thread",
  cwd: "/project-1",
  folder: "/project-1",
};
const SESSIONS = [
  session(ID, 0),
  PANEL,
  THREAD,
  ...Array.from({ length: 40 }, (_, index) =>
    session((index + 1).toString(16).padStart(32, "b"), index + 1),
  ),
];
const HISTORY = Array.from({ length: 120 }, (_, index) => ({
  id: `m-${index}`,
  kind: index % 2 ? "assistant" : "user",
  text: `Message ${index} with a sentence of history.`,
  sequence: index + 1,
}));

// Opens the saved conversation on the mocked host. In the page, testSend
// sends the host's next event for it, and testSince counts the renders and
// storage writes (by key) since testMark.
async function open(page) {
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
    if (path === `/api/sessions/${ID}`)
      return json({
        epoch: EPOCH,
        cursor: HISTORY.length,
        metadata: SESSIONS[0],
        state: { phase: "idle", view: { blocks: HISTORY } },
        pending: null,
      });
    return json({ error: "Not in this mock" }, 404);
  });
  await page.addInitScript(
    ({ epoch, cursor, id, generation }) => {
      // The host's event stream, driven by the test.
      globalThis.EventSource = class extends EventTarget {
        readyState = 1;
        constructor() {
          super();
          globalThis.testStream = this;
          setTimeout(() =>
            this.dispatchEvent(
              new MessageEvent("ready", {
                data: JSON.stringify({ epoch, cursor }),
              }),
            ),
          );
        }
        close() {
          this.readyState = 2;
        }
      };
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
      // What the page still does after it first shows (lists and settings
      // that arrive late on a busy machine) is not the test's doing:
      // counting starts once nothing has rendered for half a second.
      let marked;
      globalThis.testMark = async () => {
        for (let still = 0, seen = ""; still < 25; await testFrame()) {
          const now = JSON.stringify(globalThis.renderCounts);
          still = now === seen ? still + 1 : 0;
          seen = now;
        }
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
  await page.goto(new URL(`/#session=${ID}`, SHOWCASE_URL).href);
  await expect(page.getByLabel("Message or guidance")).toBeEnabled();
  await expect(page.locator('[data-message-id="m-119"]')).toBeVisible();
  await page.evaluate(() => import("/src/render-count.ts"));
}

// What a test counted, in its report: the numbers a budget is set from.
const measured = (type, counts) =>
  test.info().annotations.push({ type, description: JSON.stringify(counts) });

// The turn's reply streams sixty frames, with a tool call every tenth.
const streamReply = (page) =>
  page.evaluate(async () => {
    testSend({
      kind: "block",
      block: {
        id: "reply",
        response_id: "reply",
        kind: "assistant",
        text: "",
        streaming: true,
        sequence: testSequence,
      },
    });
    for (let index = 0; index < 60; index++) {
      testSend({
        kind: "block",
        id: "reply",
        append: { text: `word${index} ` },
      });
      // Every tenth frame a tool call starts and finishes: whole rows.
      if (index % 10 === 9)
        for (const status of ["running", "success"])
          testSend({
            kind: "block",
            block: {
              id: `tool-${index}`,
              kind: "tool_result",
              response_id: "reply",
              name: "read_path",
              arguments: JSON.stringify({ path: `file-${index}.ts` }),
              status,
              text: status === "success" ? "read" : "",
              sequence: testSequence,
            },
          });
      await testFrame();
    }
    testSend({ kind: "block", id: "reply", set: { streaming: false } });
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
  // The shell and its sidebar sit out the stream; each frame renders the
  // conversation once and only the rows it touched.
  expect(counts.App || 0).toBe(0);
  expect(counts.SidebarView || 0).toBe(0);
  expect(counts.SessionRow || 0).toBe(0);
  expect(counts.ChatPage).toBeLessThanOrEqual(62);
  expect(counts.MessageView).toBeLessThanOrEqual(62 + 6);
  expect(counts.Composer).toBeLessThanOrEqual(62);

  // Background work the turn started, then twenty of its progress ticks.
  await page.evaluate(async () => {
    testSend({
      kind: "block",
      block: {
        id: "spawn",
        kind: "tool_result",
        name: "spawn",
        status: "success",
        text: "started",
        parts: [{ kind: "link", to: "agent", id: "child", label: "Open" }],
        sequence: testSequence,
      },
    });
    // Failed calls keep their own rows and link to no work.
    for (let index = 0; index < 5; index++)
      testSend({
        kind: "block",
        block: {
          id: `failed-${index}`,
          kind: "tool_result",
          name: "read_path",
          status: "failed",
          text: "missing",
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
              kind: "agent",
              agent_id: "child",
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
  // A tick of background work renders the one tool row that links to it.
  await expect(page.locator('[data-message-id="spawn"]')).toContainText(
    "running",
  );
  // At most once a tick: two ticks that reach one frame are one render, as
  // happened on a busy runner where an exact count of 20 came out as 19.
  expect(ticks.ToolRow).toBeGreaterThan(0);
  expect(ticks.ToolRow).toBeLessThanOrEqual(20);
  // The sidebar renders that session's row, not the other forty.
  expect(ticks.SessionRow).toBeGreaterThan(0);
  expect(ticks.SessionRow).toBeLessThanOrEqual(20);

  // The palette fills its sheet, and a list the person scrolled stays where
  // they left it while events keep re-rendering the app behind it.
  await page.keyboard.press("ControlOrMeta+k");
  const palette = page.getByRole("dialog", { name: "Command palette" });
  const results = palette.getByRole("listbox", { name: "Results" });
  await expect(results.getByRole("option").first()).toBeVisible();
  const gap = await results.evaluate(
    (list) =>
      list.closest("dialog").getBoundingClientRect().bottom -
      list.getBoundingClientRect().bottom,
  );
  expect(gap).toBeLessThan(40);
  await results.evaluate((list) => (list.scrollTop = list.scrollHeight));
  const scrolled = await results.evaluate((list) => list.scrollTop);
  expect(scrolled).toBeGreaterThan(0);
  const renders = await page.evaluate(async () => {
    const before = globalThis.renderCounts.Palette || 0;
    for (let index = 0; index < 10; index++) {
      testSend({
        kind: "event",
        type: "activities.changed",
        data: {
          activities: [
            {
              id: 1,
              kind: "agent",
              agent_id: "child",
              status: "running",
              progress: `later ${index}`,
            },
          ],
        },
      });
      await testFrame();
    }
    return (globalThis.renderCounts.Palette || 0) - before;
  });
  // The premise: the palette did re-render under the person's scroll.
  expect(renders).toBeGreaterThan(0);
  expect(await results.evaluate((list) => list.scrollTop)).toBe(scrolled);
});

test("typing renders the composer, not the transcript or the list", async ({
  page,
}) => {
  await open(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.focus();
  await page.evaluate(() => testMark());
  await prompt.pressSequentially("twenty characters!!!");
  const { renders, writes } = await page.evaluate(() => testSince());
  measured("typed", { renders, writes });
  expect(renders.Composer).toBeGreaterThan(0);
  expect(renders.App || 0).toBeLessThanOrEqual(20);
  expect(renders.SidebarView || 0).toBeLessThanOrEqual(20);
  expect(renders.ChatPage || 0).toBeLessThanOrEqual(20);
  expect(renders.Chat || 0).toBeLessThanOrEqual(20);
  expect(writes["uagent-drafts"] || 0).toBeLessThanOrEqual(20);
});

test("state frames and an open dialog leave the shell alone", async ({
  page,
}) => {
  await open(page);
  const turn = { ...SESSIONS[0], status: "running", turn_active: true };
  // A turn's state frames: the same metadata each time, then twenty
  // activity frames that change only the caption.
  const frames = await page.evaluate(async (metadata) => {
    await testMark();
    for (let index = 0; index < 20; index++) {
      testSend({
        kind: "state",
        phase: "working",
        state: { phase: "working", activity: "Working" },
        pending: null,
        metadata: { ...metadata, activity: "Working" },
      });
      await testFrame();
    }
    const state = testSince().renders;
    await testMark();
    for (let index = 0; index < 20; index++) {
      testSend({
        kind: "activity",
        activity: `Reading file ${index}`,
        phase: "working",
        metadata: { ...metadata, activity: `Reading file ${index}` },
      });
      await testFrame();
    }
    return { state, activity: testSince().renders };
  }, turn);
  measured("frames", frames);
  expect(frames.state.App || 0).toBeLessThanOrEqual(20);
  expect(frames.state.SidebarView || 0).toBeLessThanOrEqual(20);
  expect(frames.state.Composer || 0).toBeLessThanOrEqual(20);
  expect(frames.activity.App || 0).toBeLessThanOrEqual(40);
  expect(frames.activity.SidebarView || 0).toBeLessThanOrEqual(40);
  expect(frames.activity.SessionRow || 0).toBeLessThanOrEqual(20);
  expect(frames.activity.Composer || 0).toBeLessThanOrEqual(40);

  // Settings stays open while the reply streams behind it.
  await page.getByRole("button", { name: "Settings" }).click();
  await expect(
    page.getByRole("heading", { name: "Settings", exact: true }),
  ).toBeVisible();
  await page.evaluate(() => testMark());
  await streamReply(page);
  const { renders } = await page.evaluate(() => testSince());
  measured("settings", renders);
  expect(renders.SettingsNav || 0).toBeLessThanOrEqual(62);
});
