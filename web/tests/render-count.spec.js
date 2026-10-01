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
const SESSIONS = [
  session(ID, 0),
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

test("streaming renders only what changed", async ({ page }) => {
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
    ({ epoch, cursor }) => {
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
    },
    { epoch: EPOCH, cursor: HISTORY.length },
  );
  await page.goto(new URL(`/#session=${ID}`, SHOWCASE_URL).href);
  await expect(page.getByLabel("Message or guidance")).toBeEnabled();
  await expect(page.locator('[data-message-id="m-119"]')).toBeVisible();
  await page.evaluate(() => import("/src/render-count.ts"));

  const { streamed: counts, ticks } = await page.evaluate(
    async ({ epoch, id, generation, cursor }) => {
      let sequence = cursor + 1;
      const send = (fields) =>
        globalThis.testStream.dispatchEvent(
          new MessageEvent("update", {
            data: JSON.stringify({
              v: 2,
              epoch,
              sequence: sequence++,
              session_id: id,
              generation,
              ...fields,
            }),
          }),
        );
      const frame = () => new Promise((resolve) => setTimeout(resolve, 20));
      send({
        kind: "block",
        block: {
          id: "reply",
          response_id: "reply",
          kind: "assistant",
          text: "",
          streaming: true,
          sequence: sequence,
        },
      });
      for (let index = 0; index < 60; index++) {
        send({ kind: "block", id: "reply", append: { text: `word${index} ` } });
        // Every tenth frame a tool call starts and finishes: whole rows.
        if (index % 10 === 9)
          for (const status of ["running", "success"])
            send({
              kind: "block",
              block: {
                id: `tool-${index}`,
                kind: "tool_result",
                response_id: "reply",
                name: "read_path",
                arguments: JSON.stringify({ path: `file-${index}.ts` }),
                status,
                text: status === "success" ? "read" : "",
                sequence: sequence,
              },
            });
        await frame();
      }
      send({ kind: "block", id: "reply", set: { streaming: false } });
      await frame();
      const streamed = { ...globalThis.renderCounts };
      // Background work the turn started, then twenty of its progress ticks.
      send({
        kind: "block",
        block: {
          id: "spawn",
          kind: "tool_result",
          name: "spawn",
          status: "success",
          text: "started",
          parts: [{ kind: "link", to: "agent", id: "child", label: "Open" }],
          sequence: sequence,
        },
      });
      // Failed calls keep their own rows and link to no work.
      for (let index = 0; index < 5; index++)
        send({
          kind: "block",
          block: {
            id: `failed-${index}`,
            kind: "tool_result",
            name: "read_path",
            status: "failed",
            text: "missing",
            sequence: sequence,
          },
        });
      await frame();
      const before = { ...globalThis.renderCounts };
      for (let index = 0; index < 20; index++) {
        send({
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
        await frame();
      }
      const ticks = Object.fromEntries(
        Object.entries(globalThis.renderCounts).map(([name, count]) => [
          name,
          count - (before[name] || 0),
        ]),
      );
      return { streamed, ticks };
    },
    {
      epoch: EPOCH,
      id: ID,
      generation: SESSIONS[0].generation,
      cursor: HISTORY.length,
    },
  );
  await expect(page.locator('[data-message-id="reply"]')).toContainText(
    "word59",
  );
  // The shell and its sidebar sit out the stream; each frame renders the
  // conversation once and only the rows it touched.
  expect(counts.App || 0).toBe(0);
  expect(counts.SidebarView || 0).toBe(0);
  expect(counts.SessionRow || 0).toBe(0);
  expect(counts.ChatPage).toBeLessThanOrEqual(62);
  expect(counts.MessageView).toBeLessThanOrEqual(62 + 6);
  // A tick of background work renders the one tool row that links to it.
  await expect(page.locator('[data-message-id="spawn"]')).toContainText(
    "running",
  );
  expect(ticks.ToolRow).toBe(20);
});
