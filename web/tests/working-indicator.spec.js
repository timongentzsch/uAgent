import {
  devServerTest as test,
  expect,
  DEV_SERVER_URL,
  fakeStream,
  mockHost,
} from "./fixtures.js";

// The footer shows ongoing work. In the sidebar a row's left line is its
// state: only the open working row's line moves; the others stand still.
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
  session(6, { needs_you: true }),
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
  await mockHost(page, {
    epoch: EPOCH,
    cursor: 10,
    sessions: SESSIONS,
    open: OPEN,
    state: { phase: "working", turn_status: "Working", view: { nodes: NODES } },
  });
  await page.addInitScript(fakeStream, { epoch: EPOCH, cursor: 10 });
  await page.addInitScript(
    ({ epoch, open }) => {
      // No worker here: its failed registration's banner covers the page.
      delete Navigator.prototype.serviceWorker;
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
// The list's running animations: each its row's title and pseudo-element.
const listAnimations = (page) =>
  page.evaluate(() =>
    document
      .getAnimations()
      .filter((animation) =>
        animation.effect?.target?.closest?.(".sidebar, dialog.drawer"),
      )
      .map(
        (animation) =>
          `${animation.effect.target
            .querySelector(".row-label")
            ?.textContent.replace(/Working|Failed|Needs your input/, "")
            .trim()} ${animation.effect.pseudoElement}`,
      ),
  );
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

test("the working footer and selected sidebar mark move while background rows stay still", async ({
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
  // In the list only the open row's line moves.
  expect(await listAnimations(page)).toEqual([`Conversation 1 ${"::after"}`]);
  expect(await workAnimations(page)).toBe(1);

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

// A row's state as its left line, and the row itself.
const row = (page, n) =>
  page
    .locator(".session-row")
    .filter({ hasText: `Conversation ${n}` })
    .first();
const line = (locator) =>
  locator.evaluate((node) => {
    const style = (pseudo) => getComputedStyle(node, pseudo);
    const box = node.getBoundingClientRect();
    const label = node.querySelector(".row-label").getBoundingClientRect();
    const button = getComputedStyle(node.querySelector(".session"));
    return {
      track:
        style("::before").content === "none"
          ? "none"
          : style("::before").backgroundImage.startsWith("repeating")
            ? "dotted"
            : style("::before").backgroundColor,
      run:
        style("::after").content === "none"
          ? "none"
          : style("::after").animationName,
      width: Number.parseFloat(style("::before").width) || 0,
      // The title starts at the row's own inset: no column before it.
      title: Math.round(label.left - box.left),
      cut:
        node.querySelector(".row-label").scrollWidth >
        node.querySelector(".row-label").clientWidth,
      inset: Math.round(Number.parseFloat(button.paddingLeft)),
    };
  });

for (const width of [1280, 390])
  test(`each row's state is its left line and its title starts at the row's inset (${width})`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: 800 });
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await open(page);
    if (width < 600)
      await page.getByRole("button", { name: "Open conversations" }).click();
    const colours = await page.evaluate(() => {
      const probe = document.createElement("div");
      document.body.append(probe);
      const of = (token) => {
        probe.style.color = `var(${token})`;
        return getComputedStyle(probe).color;
      };
      const found = { accent: of("--accent"), removed: of("--removed") };
      probe.remove();
      return found;
    });
    const stripe = await page.evaluate(() =>
      Number.parseFloat(
        getComputedStyle(document.documentElement).getPropertyValue("--stripe"),
      ),
    );
    expect(await line(row(page, 1))).toMatchObject({
      track: "dotted",
      run: "line-run",
      width: stripe,
    });
    expect(await line(row(page, 2))).toMatchObject({
      track: "dotted",
      run: "none",
    });
    expect(await line(row(page, 4))).toMatchObject({
      track: colours.removed,
      run: "none",
    });
    expect(await line(row(page, 5))).toMatchObject({
      track: "none",
      run: "none",
    });
    expect(await line(row(page, 6))).toMatchObject({
      track: colours.accent,
      run: "none",
    });
    for (const n of [1, 2, 3, 4, 5, 6]) {
      const { title, inset, cut } = await line(row(page, n));
      expect(title).toBe(inset);
      expect(cut).toBe(false);
    }
    // The open row is told by its fill, not by a stripe of its own.
    expect(
      await row(page, 1).evaluate((node) => getComputedStyle(node).boxShadow),
    ).toBe("none");
    // The state is said, as it was by the column.
    await expect(row(page, 4)).toContainText("Failed");
    await expect(row(page, 6)).toContainText("Needs your input");
    await expect(row(page, 2)).toContainText("Working");
    // The drawer's own entrance has played by then.
    await expect
      .poll(() => listAnimations(page))
      .toEqual([`Conversation 1 ${"::after"}`]);
  });

test("with motion reduced no line moves", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await open(page);
  expect(await line(row(page, 1))).toMatchObject({ track: "dotted" });
  expect(await listAnimations(page)).toEqual([]);
});

// A step at work looks as it will when it is done: its arrow stays, it
// carries no mark, and its words do not move when it finishes.
for (const width of [390, 1280])
  test(`a step keeps its arrow and its place while it works (${width})`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: 800 });
    await open(page);
    const row = (id) => page.locator(`[data-message-id="${id}"] > .node-row`);
    const words = (id) =>
      row(id)
        .locator(".node-headline")
        .evaluate((node) => node.getBoundingClientRect().left);
    await expect(row("w").locator(".row-chevron")).toBeVisible();
    await expect(row("w").locator(".status-led")).toHaveCount(0);
    await row("w").click();
    await expect(row("w")).toHaveAttribute("aria-expanded", "true");
    await expect(row("c2").locator(".status-led")).toHaveCount(0);
    const before = { group: await words("w"), step: await words("c2") };
    expect(before.step).toBe(await words("c1"));
    await page.evaluate(() => {
      testSend({
        kind: "node",
        node: {
          id: "c2",
          parent: "w",
          kind: "call",
          state: "ok",
          headline: { verb: "Read", target: "b.ts" },
          body: { kind: "text", text: "read" },
        },
      });
      testSend({
        kind: "node",
        node: {
          id: "w",
          parent: "u",
          kind: "work",
          state: "ok",
          headline: { verb: "", target: "2 steps" },
        },
      });
    });
    await expect(row("c2").locator(".node-headline")).toContainText(
      "Read b.ts",
    );
    await expect(row("w").locator(".row-chevron")).toBeVisible();
    expect(await words("w")).toBe(before.group);
    expect(await words("c2")).toBe(before.step);
  });
