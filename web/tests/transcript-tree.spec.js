import {
  devServerTest as test,
  expect,
  DEV_SERVER_URL,
  openMore,
} from "./fixtures.js";
import { agents, arrivals, conversation, pondering } from "./golden-turns.js";

// The transcript tree on the dev page with a mocked host, at each detail
// level. Set UAGENT_SHOTS to a folder to keep a screenshot of each.
test.use({ serviceWorkers: "block" });

const ID = "a".repeat(32);
const SESSION = {
  id: ID,
  generation: "g",
  title: "Golden turns",
  cwd: "/project",
  status: "idle",
  presence: "active",
  updated: 1_700_000_000_000,
};

async function open(page, level, nodes = conversation) {
  await page.addInitScript((detail) => {
    localStorage.setItem("uagent-detail", detail);
    localStorage.setItem("uagent-thinking", "show");
    globalThis.EventSource = class extends EventTarget {
      readyState = 1;
      constructor() {
        super();
        setTimeout(() =>
          this.dispatchEvent(
            new MessageEvent("ready", {
              data: JSON.stringify({ epoch: "e", cursor: 1 }),
            }),
          ),
        );
      }
      close() {
        this.readyState = 2;
      }
    };
  }, level);
  await page.route("**/api/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    const json = (body, status = 200) =>
      route.fulfill({ status, json: { v: 2, ...body } });
    if (path === "/api/sessions")
      return json({
        epoch: "e",
        cursor: 1,
        sessions: [SESSION],
        capabilities: {},
        devices: [],
        commands: [],
      });
    if (path === `/api/sessions/${ID}`)
      return json({
        epoch: "e",
        cursor: 1,
        metadata: SESSION,
        state: { phase: "idle", view: { nodes } },
        pending: null,
      });
    return json({ error: "Not in this mock" }, 404);
  });
  await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
  await expect(
    page.locator(`[data-message-id="${nodes[0].id}"]`),
  ).toBeVisible();
}

const rows = (page) => page.locator(".transcript .node-row");

// The vertical space between each pair of neighbours in every column of the
// transcript: the conversation, and what an open row holds.
const gaps = (page) =>
  page.evaluate(() => {
    const found = [];
    for (const column of document.querySelectorAll(
      ".transcript-content, .node, .node-open, .node-kids, .node-body",
    )) {
      // What is out of the flow (a spoken-only line) has no place in it.
      const shown = [...column.children]
        .filter((child) => getComputedStyle(child).position !== "absolute")
        .map((child) => child.getBoundingClientRect())
        .filter((box) => box.height > 0);
      for (let index = 1; index < shown.length; index++)
        found.push(
          Math.round((shown[index].top - shown[index - 1].bottom) * 10) / 10,
        );
    }
    return found;
  });

for (const [device, viewport] of [
  // Tall enough to hold the whole conversation open at full.
  ["desktop", { width: 1280, height: 1400 }],
  ["phone", { width: 390, height: 1800 }],
])
  for (const level of ["minimal", "default", "full"])
    test(`rows are evenly spaced at ${level} on a ${device}`, async ({
      page,
    }) => {
      await page.setViewportSize(viewport);
      await open(page, level);
      const shown = rows(page);
      // Minimal shows each turn's work as one line; default opens it.
      await expect(shown).toHaveCount(
        { minimal: 4, default: 13, full: 13 }[level],
      );
      const measured = await gaps(page);
      expect(measured.length).toBeGreaterThan(0);
      expect(new Set(measured).size, JSON.stringify(measured)).toBe(1);
      const heights = await shown.evaluateAll((items) =>
        items.map((row) => Math.round(row.getBoundingClientRect().height)),
      );
      expect(new Set(heights).size, JSON.stringify(heights)).toBe(1);
      if (process.env.UAGENT_SHOTS) {
        // The mock has no service worker, which the page reports.
        await page.getByRole("button", { name: "Dismiss" }).click();
        await page.locator(".transcript").screenshot({
          path: `${process.env.UAGENT_SHOTS}/${device}-${level}.png`,
        });
      }
    });

test("a long result word gives way to the headline on a phone", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await open(page, "default");
  const rejected = page.locator('[data-message-id="t-j"]');
  const row = rejected.locator(":scope > .node-row");
  const [headline, word] = await Promise.all(
    [".node-headline", ".node-word"].map((selector) =>
      row.locator(selector).evaluate((element) => ({
        width: element.getBoundingClientRect().width,
        cut: element.scrollWidth > element.clientWidth,
      })),
    ),
  );
  expect(headline.cut).toBe(false);
  expect(headline.width).toBeGreaterThan(word.width);
  expect(word.cut).toBe(true);
  await expect(row.locator(".node-word")).toHaveCSS(
    "text-overflow",
    "ellipsis",
  );
  await row.click();
  await expect(rejected.locator(".node-body")).toContainText(
    "path is outside the folder: /etc is not inside /project",
  );
});

test("rows open and close from the keyboard, and stay as left", async ({
  page,
}) => {
  await open(page, "minimal");
  const work = page.locator('[data-message-id="w-m-22"] > .node-row');
  await work.focus();
  await page.keyboard.press("ArrowRight");
  await expect(work).toHaveAttribute("aria-expanded", "true");
  await page.keyboard.press("ArrowRight");
  const first = page.locator('[data-message-id="t-f"] > .node-row');
  await expect(first).toBeFocused();
  await page.keyboard.press("ArrowLeft");
  await expect(work).toBeFocused();
  await page.keyboard.press("Space");
  await expect(work).toHaveAttribute("aria-expanded", "false");
  await page.keyboard.press("Enter");
  await page.reload();
  await expect(work).toHaveAttribute("aria-expanded", "true");
  await expect(
    page.locator('[data-message-id="w-m-12"] > .node-row'),
  ).toHaveAttribute("aria-expanded", "false");
  await page
    .locator(".conversation-head")
    .getByLabel("Conversation menu", { exact: true })
    .click();
  await openMore(page);
  await page.getByRole("menuitem", { name: /^Detail/ }).click();
  await page.getByRole("menuitemradio", { name: "Full" }).click();
  await expect(rows(page)).toHaveCount(13);
  await page
    .locator(".conversation-head")
    .getByLabel("Conversation menu", { exact: true })
    .click();
  await openMore(page);
  await page.getByRole("menuitem", { name: /^Detail/ }).click();
  await page.getByRole("menuitemradio", { name: "Minimal" }).click();
  await expect(work).toHaveAttribute("aria-expanded", "false");
});

test("a thought is text among the steps: one line under a folded work, two and the rest on asking at default, whole at full", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1280, height: 900 });
  await open(page, "minimal", pondering);
  const thought = page.locator(".thought");
  const text = thought.locator(".node-text");
  const lines = async () => {
    const { height, line } = await text.evaluate((node) => ({
      height: node.getBoundingClientRect().height,
      line: parseFloat(getComputedStyle(node).lineHeight),
    }));
    return Math.round(height / line);
  };
  // Not a row: nothing to open, no box round it, and set apart by its face.
  await expect(thought.locator(".node-row")).toHaveCount(0);
  await expect(text).toHaveCSS("font-style", "italic");
  await expect(text).toHaveCSS("border-top-width", "0px");
  // Folded, the work still says what it is thinking, on one line.
  await expect(
    page.locator('[data-message-id="w-m-40"] > .node-row'),
  ).toHaveAttribute("aria-expanded", "false");
  expect(await lines()).toBe(1);
  const head = page.locator(".conversation-head");
  await head.getByRole("tab", { name: "Default" }).click();
  await expect(thought).toHaveCount(1);
  expect(await lines()).toBe(2);
  await thought.getByRole("button", { name: "More" }).click();
  expect(await lines()).toBeGreaterThan(2);
  await thought.getByRole("button", { name: "Less" }).click();
  expect(await lines()).toBe(2);
  await head.getByRole("tab", { name: "Full" }).click();
  await expect(thought.getByRole("button", { name: "More" })).toHaveCount(0);
  expect(await lines()).toBeGreaterThan(2);
});

test("the header sets the level and shows or hides thinking", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1280, height: 1400 });
  await open(page, "full");
  await expect(rows(page)).toHaveCount(13);
  const head = page.locator(".conversation-head");
  const thinking = head.getByRole("button", { name: "Thinking", exact: true });
  await expect(thinking).toHaveAttribute("aria-pressed", "true");
  // A thought is text among the rows, so hiding it leaves them as they were.
  await expect(page.locator('[data-kind="thinking"]')).toHaveCount(1);
  await thinking.click();
  await expect(thinking).toHaveAttribute("aria-pressed", "false");
  await expect(rows(page)).toHaveCount(13);
  await expect(page.locator('[data-kind="thinking"]')).toHaveCount(0);
  await head.getByRole("tab", { name: "Minimal" }).click();
  await expect(
    page.locator('[data-message-id="w-m-22"] > .node-row'),
  ).toHaveAttribute("aria-expanded", "false");
});

test("what the agent says while it works shows under a folded work", async ({
  page,
}) => {
  // open() waits for r-3-2-1, here the note.
  const running = [
    {
      id: "m-1",
      parent: null,
      kind: "turn",
      body: { kind: "text", text: "q" },
    },
    {
      id: "w-m-1",
      parent: "m-1",
      kind: "work",
      state: "running",
      headline: { verb: "", target: "1 read" },
    },
    {
      id: "t-1",
      parent: "w-m-1",
      kind: "call",
      state: "ok",
      headline: { verb: "Read", target: "a.txt" },
    },
    {
      id: "r-3-2-1",
      parent: "w-m-1",
      kind: "message",
      role: "assistant",
      body: { kind: "markdown", text: "Looking at the host first." },
    },
  ];
  await open(page, "minimal", running);
  const work = page.locator('[data-message-id="w-m-1"]');
  await expect(work.locator("> .node-row")).toHaveAttribute(
    "aria-expanded",
    "false",
  );
  await expect(work).toContainText("Looking at the host first.");
  await expect(page.locator('[data-message-id="t-1"]')).toHaveCount(0);
});

test("an agent's link opens its conversation", async ({ page }) => {
  await open(page, "full", [...conversation, ...agents]);
  await page.getByRole("button", { name: "Open agent", exact: true }).click();
  await expect.poll(() => new URL(page.url()).hash).toBe("#session=child");
  await expect(page.locator("dialog[open]")).toHaveCount(0);
});

test("an agent's report and finished background work read as events", async ({
  page,
}) => {
  await open(page, "default", [...conversation, ...arrivals]);
  const report = page.getByRole("article", { name: /^Agent report/ });
  await expect(report).toContainText("Agent “Find flights” finished");
  await expect(report).toContainText("Two flights found.");
  await expect(report).not.toContainText("[agent");
  const background = page.getByRole("article", { name: /^Background result/ });
  await expect(background).toContainText("A background command finished");
  await expect(background).not.toContainText("not a user message");
  await report.getByRole("button", { name: "Open agent", exact: true }).click();
  await expect.poll(() => new URL(page.url()).hash).toBe("#session=child");
});
