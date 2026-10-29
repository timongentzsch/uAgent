import { devServerTest as test, expect, DEV_SERVER_URL } from "./fixtures.js";
import { agents, conversation } from "./golden-turns.js";

// The transcript tree on the Vite-served app with a mocked host: a saved
// conversation of calls, a failure, a group, guidance and an answer, at each
// detail level, on a phone and a desktop. Set UAGENT_SHOTS to a folder to
// keep a screenshot of each.
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
  await expect(page.locator('[data-message-id="r-3-2-1"]')).toBeVisible();
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
        { minimal: 4, default: 13, full: 15 }[level],
      );
      const measured = await gaps(page);
      expect(measured.length).toBeGreaterThan(0);
      expect(new Set(measured).size, JSON.stringify(measured)).toBe(1);
      // Every row is one line of the same height.
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
  // The headline reads whole and is the wider; the word is the one cut.
  expect(headline.cut).toBe(false);
  expect(headline.width).toBeGreaterThan(word.width);
  expect(word.cut).toBe(true);
  await expect(row.locator(".node-word")).toHaveCSS(
    "text-overflow",
    "ellipsis",
  );
  // Opened, the row says the whole of why.
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
  // → again moves to the first row under it; ← closes it from there back.
  await page.keyboard.press("ArrowRight");
  const first = page.locator('[data-message-id="t-f"] > .node-row');
  await expect(first).toBeFocused();
  await page.keyboard.press("ArrowLeft");
  await expect(work).toBeFocused();
  await page.keyboard.press("Space");
  await expect(work).toHaveAttribute("aria-expanded", "false");
  await page.keyboard.press("Enter");
  // The row stays open after a reload; choosing a level forgets it.
  await page.reload();
  await expect(work).toHaveAttribute("aria-expanded", "true");
  await expect(
    page.locator('[data-message-id="w-m-12"] > .node-row'),
  ).toHaveAttribute("aria-expanded", "false");
  await page
    .locator(".conversation-head")
    .getByLabel("Conversation menu", { exact: true })
    .click();
  await page.getByRole("menuitem", { name: /^Detail/ }).click();
  await page.getByRole("menuitemradio", { name: "Full" }).click();
  await expect(rows(page)).toHaveCount(15);
  await page
    .locator(".conversation-head")
    .getByLabel("Conversation menu", { exact: true })
    .click();
  await page.getByRole("menuitem", { name: /^Detail/ }).click();
  await page.getByRole("menuitemradio", { name: "Minimal" }).click();
  await expect(work).toHaveAttribute("aria-expanded", "false");
});

test("an agent's link opens its conversation", async ({ page }) => {
  await open(page, "full", [...conversation, ...agents]);
  await page.getByRole("button", { name: "Open agent", exact: true }).click();
  await expect.poll(() => new URL(page.url()).hash).toBe("#session=child");
  await expect(page.locator("dialog[open]")).toHaveCount(0);
});
