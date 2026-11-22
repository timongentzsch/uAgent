import { devServerTest as test, expect, DEV_SERVER_URL } from "./fixtures.js";

// Work is shown by one moving thing, the band in the status line under the
// input; everything else that says "at work" stands still, as a square.
const EPOCH = "work-epoch";
const id = (n) => n.toString(16).padStart(32, "0");
const NOW = Date.now();
const session = (n, fields = {}) => ({
  id: id(n),
  generation: `g-${n}`,
  title: `Conversation ${n}`,
  cwd: "/project",
  status: "idle",
  presence: "active",
  updated: NOW - n * 60_000,
  turns: 1,
  ...fields,
});
const OPEN = session(1, { turn_active: true, status: "running" });
const SESSIONS = [
  OPEN,
  session(2, { turn_active: true, status: "running" }),
  session(3, { incoming: 2 }),
  session(4, { error: "The model refused." }),
  session(5),
];
const NODES = [
  { id: "u", parent: null, kind: "turn", body: { kind: "text", text: "Go" } },
  {
    id: "w",
    parent: "u",
    kind: "work",
    state: "running",
    headline: { verb: "", target: "2 steps" },
  },
  {
    id: "c1",
    parent: "w",
    kind: "call",
    state: "ok",
    headline: { verb: "Read", target: "a.ts" },
    body: { kind: "text", text: "read" },
  },
  {
    id: "c2",
    parent: "w",
    kind: "call",
    state: "running",
    headline: { verb: "Reading", target: "b.ts" },
  },
];

async function open(page) {
  await page.route("**/api/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    const json = (body) => route.fulfill({ json: { v: 2, ...body } });
    if (path === "/api/sessions")
      return json({
        epoch: EPOCH,
        cursor: 10,
        sessions: SESSIONS,
        capabilities: {},
        devices: [],
        commands: [],
      });
    if (path === `/api/sessions/${OPEN.id}`)
      return json({
        epoch: EPOCH,
        cursor: 10,
        metadata: OPEN,
        state: {
          phase: "working",
          turn_status: "Working",
          view: { nodes: NODES },
        },
        pending: null,
      });
    return route.fulfill({ status: 404, json: { v: 2, error: "no" } });
  });
  await page.addInitScript(
    ({ epoch, open }) => {
      globalThis.EventSource = class extends EventTarget {
        readyState = 1;
        constructor() {
          super();
          globalThis.testStream = this;
          setTimeout(() =>
            this.dispatchEvent(
              new MessageEvent("ready", {
                data: JSON.stringify({ epoch, cursor: 10 }),
              }),
            ),
          );
        }
        close() {}
      };
      let sequence = 11;
      globalThis.testSend = (fields) =>
        globalThis.testStream.dispatchEvent(
          new MessageEvent("update", {
            data: JSON.stringify({
              v: 2,
              epoch,
              sequence: sequence++,
              session_id: open.id,
              generation: open.generation,
              ...fields,
            }),
          }),
        );
    },
    { epoch: EPOCH, open: OPEN },
  );
  await page.goto(new URL(`/#session=${OPEN.id}`, DEV_SERVER_URL).href);
  await expect(page.locator('[data-message-id="w"]')).toBeVisible();
}

const band = (page) => page.locator("canvas.work-band");
// The band's pixels, as an image.
const pixels = (page) => band(page).evaluate((canvas) => canvas.toDataURL());
// What runs an animation because of work: none should.
const workAnimations = (page) =>
  page.evaluate(
    () =>
      document
        .getAnimations()
        .filter((animation) =>
          animation.effect?.target?.closest?.(
            ".status-led, .node-headline, .node-row, .session-row, .todo-items, .chip, .activity-button",
          ),
        ).length,
  );

test("while a turn works, the status line's band is the one thing that moves", async ({
  page,
}) => {
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await open(page);
  await expect(band(page)).toHaveCount(1);
  await expect(page.locator(".activity-button canvas.work-band")).toHaveCount(
    1,
  );
  await expect(page.locator(".activity-button")).toContainText("Working");
  // The glow travels: the band's pixels change.
  const first = await pixels(page);
  await expect.poll(() => pixels(page)).not.toBe(first);
  expect(await workAnimations(page)).toBe(0);
  // The running step is told apart by a still square, a finished one has none.
  await expect(
    page.locator('[data-message-id="w"] > .node-row .status-led.running'),
  ).toHaveCount(1);

  // Done: the band goes with the work.
  await page.evaluate(
    (metadata) =>
      testSend({
        kind: "state",
        phase: "idle",
        state: { phase: "idle" },
        pending: null,
        metadata,
      }),
    { ...OPEN, turn_active: false, status: "idle" },
  );
  await expect(band(page)).toHaveCount(0);
});

test("a decision waiting stops the band", async ({ page }) => {
  await open(page);
  await expect(band(page)).toHaveCount(1);
  await page.evaluate(
    (metadata) =>
      testSend({
        kind: "state",
        phase: "waiting",
        state: { phase: "waiting" },
        pending: {
          kind: "ask",
          interaction_id: "q",
          questions: [{ id: "a", question: "Which one?", options: [] }],
        },
        metadata,
      }),
    { ...OPEN, needs_you: true },
  );
  await expect(page.locator(".activity-state")).toContainText("Needs you");
  await expect(band(page)).toHaveCount(0);
});

test("with motion off the band stands still", async ({ page }) => {
  await open(page);
  await expect(band(page)).toHaveCount(1);
  const before = await pixels(page);
  // Thirty painted frames later it is the same picture.
  await page.evaluate(
    () =>
      new Promise((done) => {
        let frames = 30;
        const tick = () => (--frames ? requestAnimationFrame(tick) : done());
        requestAnimationFrame(tick);
      }),
  );
  expect(await pixels(page)).toBe(before);
  expect(await workAnimations(page)).toBe(0);
});

test("a list's marks are squares told apart by their fill", async ({
  page,
}) => {
  // Places: a folder's head carries its unread mark.
  await page.addInitScript(() =>
    localStorage.setItem("uagent-list", JSON.stringify("places")),
  );
  await open(page);
  const row = (n) =>
    page
      .locator(".session-row")
      .filter({ hasText: `Conversation ${n}` })
      .first();
  const mark = (locator) =>
    locator.evaluate((node) => {
      const style = getComputedStyle(node);
      return {
        image: style.backgroundImage.split("(")[0],
        fill: style.backgroundColor,
        width: node.getBoundingClientRect().width,
        radius: style.borderTopLeftRadius,
      };
    });
  const working = await mark(row(2).locator(".status-led.running"));
  // A failure is its own sign; an idle row shows none.
  await expect(row(4).getByRole("img", { name: "Failed" })).toBeVisible();
  await expect(row(5).locator(".status-led")).toHaveCount(0);
  // Folded, the folder's head says that something in it is unread.
  await page.getByRole("button", { name: "Conversations of project" }).click();
  const unread = await mark(page.locator(".place-head .status-led.unread"));
  const connected = await mark(page.locator(".connection .status-led.active"));
  expect(working.image).toBe("repeating-conic-gradient");
  for (const solid of [unread, connected]) {
    expect(solid.image).toBe("none");
    expect(solid.fill).not.toBe("rgba(0, 0, 0, 0)");
  }
  expect(unread.fill).not.toBe(connected.fill);
  for (const square of [working, unread, connected]) {
    expect(square.radius).toBe("0px");
    expect(square.width).toBeCloseTo(8, 0);
  }
  expect(await workAnimations(page)).toBe(0);
});
