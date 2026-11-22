import { devServerTest as test, expect, DEV_SERVER_URL } from "./fixtures.js";

// The Statistics sheet's Context section on a mocked host, whose answer to
// context_parts waits until the test lets it go.
const ID = "c".repeat(32);
const SESSION = {
  id: ID,
  generation: "g-context",
  title: "Context",
  cwd: "/project",
  status: "idle",
  presence: "active",
  updated: Date.now(),
};
const BYTES = {
  tool_results: 164800,
  replies: 75600,
  messages: 24400,
  files: 9600,
  notes: 3200,
  tools: 17200,
  mcp: 6400,
  system: 4800,
  instructions: 2000,
  memory: 1200,
  skills: 800,
};
const bytes = Object.values(BYTES).reduce((sum, value) => sum + value, 0);
const tool = (name, size, extra = {}) => ({
  name,
  bytes: size,
  offered: true,
  ...extra,
});
const PARTS = {
  tokens: bytes / 4,
  bytes,
  window: 200000,
  compacts_at: 138000,
  parts: BYTES,
  tools: [
    tool("read_path", 1184),
    tool("apply_patch", 2448),
    tool("run", 2192),
    tool("skill", 800),
    tool("github_create_issue", 1552, { server: "github" }),
    tool("github_get_file", 1008, { server: "github" }),
    tool("browser", 1608, { offered: false }),
  ],
  skills: 4,
  instructions: "AGENTS.md",
};

async function open(page) {
  const sent = [];
  let release;
  const answered = new Promise((resolve) => {
    release = resolve;
  });
  await page.route("**/sw.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "" }),
  );
  await page.route("**/api/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    const json = (body) => route.fulfill({ json: { v: 2, ...body } });
    if (path === "/api/sessions")
      return json({
        epoch: "context",
        cursor: 1,
        sessions: [SESSION],
        capabilities: {},
        devices: [],
        commands: [],
      });
    if (path === `/api/sessions/${ID}`)
      return json({
        epoch: "context",
        cursor: 1,
        metadata: SESSION,
        state: {
          phase: "idle",
          context_tokens: PARTS.tokens,
          context_window: PARTS.window,
          turns: 14,
          view: { nodes: [] },
        },
        pending: null,
      });
    if (path === "/api/command") {
      const body = route.request().postDataJSON();
      sent.push(body);
      if (body.kind === "context_parts") await answered;
      return json({
        request_id: body.request_id,
        accepted: true,
        result: body.kind === "context_parts" ? PARTS : {},
      });
    }
    return route.fulfill({ status: 404, json: { error: "Not in this mock" } });
  });
  await page.addInitScript(() => {
    globalThis.EventSource = class extends EventTarget {
      readyState = 1;
      constructor() {
        super();
        setTimeout(() =>
          this.dispatchEvent(
            new MessageEvent("ready", {
              data: JSON.stringify({ epoch: "context", cursor: 1 }),
            }),
          ),
        );
      }
      close() {
        this.readyState = 2;
      }
    };
  });
  await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
  await page
    .getByRole("button", { name: "Conversation statistics", exact: true })
    .click();
  const sheet = page.getByRole("dialog", { name: "Conversation statistics" });
  await expect(sheet.getByText("estimated", { exact: true })).toBeVisible();
  return { sheet, sent, release };
}

// Every row's place in the sheet, the Conversation rows after them included.
const places = (sheet) =>
  sheet.evaluate((node) =>
    [...node.querySelectorAll(".context-rows > li, .stats > div")].map(
      (row) => {
        const box = row.getBoundingClientRect();
        return [Math.round(box.top), Math.round(box.height)];
      },
    ),
  );

test("the parts arrive in the rows drawn for them", async ({ page }) => {
  const { sheet, release } = await open(page);
  await expect(
    sheet.getByRole("status", { name: "Loading the parts…" }),
  ).toBeAttached();
  const before = await places(sheet);
  release();
  await expect(sheet.getByText("Tool results")).toBeVisible();
  expect(await places(sheet)).toEqual(before);
});

test("rows, bar and folds show what fills the window", async ({ page }) => {
  const { sheet, sent, release } = await open(page);
  release();
  const row = (name) => sheet.locator(".context-rows > li", { hasText: name });
  await expect(row("Tool results")).toContainText("41.2k");
  await expect(row("Tool results")).toContainText("20.6%");
  await expect(row("Instructions")).toContainText("AGENTS.md");
  await expect(row("Skills")).toContainText("4 listed");
  await expect(row("Free until it compacts")).toContainText("60.5k");
  await expect(row("Kept for the summary")).toContainText("31%");
  await expect(
    sheet.getByRole("img", {
      name: "38.8% of the context window is used, 61% left; it compacts at 69%",
    }),
  ).toBeVisible();
  await expect(sheet.getByText("61% left", { exact: true })).toBeVisible();
  await expect(sheet.getByText("77.5k of 200k tokens")).toBeVisible();

  // The mark stands at 69% of the bar and its caption ends before it.
  const geometry = await sheet.evaluate((node) => {
    const box = (selector) =>
      node.querySelector(selector).getBoundingClientRect();
    const range = document.createRange();
    range.selectNodeContents(node.querySelector(".context-caption"));
    return {
      track: box(".context-track"),
      mark: box(".context-mark"),
      caption: range.getBoundingClientRect(),
    };
  });
  expect(geometry.mark.left - geometry.track.left).toBeCloseTo(
    geometry.track.width * 0.69,
    0,
  );
  expect(geometry.caption.right).toBeLessThanOrEqual(geometry.mark.left);

  // A fold lists its tools biggest first and leaves the sheet's top alone.
  const top = await sheet.evaluate((node) => node.getBoundingClientRect().top);
  const tools = sheet.getByRole("button", { name: /^Tools 3/ });
  await expect(tools).toHaveAttribute("aria-expanded", "false");
  await tools.click();
  await expect(tools).toHaveAttribute("aria-expanded", "true");
  await expect(sheet.locator(".context-tools li")).toHaveText([
    /^apply_patch\s*612$/,
    /^run\s*548$/,
    /^read_path\s*296$/,
  ]);
  await sheet.getByRole("button", { name: /^MCP tools 2/ }).click();
  await expect(sheet.getByText("github · create_issue")).toBeVisible();
  await sheet.getByRole("button", { name: /^Not offered 1 tool/ }).click();
  await expect(sheet.getByText("browser", { exact: true })).toBeVisible();
  expect(await sheet.evaluate((node) => node.getBoundingClientRect().top)).toBe(
    top,
  );

  await sheet.getByRole("button", { name: "Compact now" }).click();
  await expect
    .poll(() => sent.find((body) => body.kind === "submit"))
    .toMatchObject({
      session_id: ID,
      generation: "g-context",
      text: "/compact",
    });
});

test("on a phone every row fits on its line", async ({ page }) => {
  const { sheet, release } = await open(page);
  release();
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(sheet.getByText("Tool results")).toBeVisible();
  await sheet.getByRole("button", { name: /^Tools 3/ }).click();
  const fit = await sheet.evaluate((node) => ({
    overflow: node.scrollWidth - node.clientWidth,
    // A row one line tall: its label never wraps under the numbers.
    tall: [...node.querySelectorAll(".context-row")]
      .filter((row) => row.getBoundingClientRect().height > 48)
      .map((row) => row.textContent),
    // The actions end the section, side by side and as wide as each other.
    actions: [...node.querySelectorAll(".context-actions > button")].map(
      (button) => {
        const box = button.getBoundingClientRect();
        return [Math.round(box.top), Math.round(box.width)];
      },
    ),
    last: node.querySelector(".context > :last-child").className,
  }));
  expect(fit.overflow).toBe(0);
  expect(fit.tall).toEqual([]);
  expect(fit.actions[0]).toEqual(fit.actions[1]);
});
