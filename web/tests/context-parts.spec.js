import {
  devServerTest as test,
  expect,
  DEV_SERVER_URL,
  fakeStream,
} from "./fixtures.js";

// The Statistics sheet's Context section on a mocked host. The parts come
// with the conversation's state; its answer to the tools catalogue, which a
// fold asks for, waits until the test lets it go.
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
  tools: 18000,
  mcp: 6400,
  system: 4800,
  instructions: 2000,
  memory: 1200,
};
const bytes = Object.values(BYTES).reduce((sum, value) => sum + value, 0);
const PARTS = {
  tokens: bytes / 4,
  bytes,
  compacts_at: 138000,
  instructions: "AGENTS.md",
  parts: BYTES,
  tools: {
    tools: { count: 4, bytes: 6624 },
    mcp: { count: 2, bytes: 2560 },
    hidden: { count: 1, bytes: 1608 },
  },
};
const tool = (name, size, extra = {}) => ({
  name,
  title: name,
  description: "",
  category: "workspace",
  provider: "builtin",
  active: true,
  available: true,
  schema_bytes: size,
  ...extra,
});
const mcp = { category: "mcp", provider: "mcp:github" };
const CATALOGUE = {
  profile: "default",
  base_profile: "default",
  profiles: ["default"],
  active: 6,
  available: 7,
  schema_bytes: 24400,
  full_schema_bytes: 26008,
  tools: [
    tool("read_path", 3217),
    tool("apply_patch", 6652),
    tool("run", 5956),
    tool("skill", 2175),
    tool("github_create_issue", 3900, mcp),
    tool("github_get_file", 2500, mcp),
    tool("browser", 1608, { active: false }),
  ],
};

async function open(page, live = PARTS.tokens, parts = PARTS) {
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
          context_tokens: live,
          context_window: 200000,
          context_parts: parts,
          turns: 14,
          view: { nodes: [] },
        },
        pending: null,
      });
    if (path === "/api/command") {
      const body = route.request().postDataJSON();
      sent.push(body);
      if (body.kind === "tools") await answered;
      return json({
        request_id: body.request_id,
        accepted: true,
        result: body.kind === "tools" ? CATALOGUE : {},
      });
    }
    return route.fulfill({ status: 404, json: { error: "Not in this mock" } });
  });
  await page.addInitScript(fakeStream, { epoch: "context", cursor: 1 });
  await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
  await page
    .getByRole("button", { name: "Conversation statistics", exact: true })
    .click();
  const sheet = page.getByRole("dialog", { name: "Conversation statistics" });
  await expect(sheet.getByText("estimated", { exact: true })).toBeVisible();
  return { sheet, sent, release };
}

test("the parts are drawn with the sheet, and a fold reads its list", async ({
  page,
}) => {
  const { sheet, sent, release } = await open(page);
  await expect(sheet.getByText("Tool results")).toBeVisible();
  await expect(sheet.locator("[aria-busy]")).toHaveCount(0);
  expect(sent).toEqual([]);
  const tools = sheet.getByRole("button", { name: /^Tools 4/ });
  await tools.click();
  await expect(sheet.locator(".context-tools")).toHaveText("Loading the list…");
  release();
  await expect(sheet.locator(".context-tools li")).toHaveText([
    /^apply_patch\s*1.7k$/,
    /^run\s*1.5k$/,
    /^read_path\s*804$/,
    /^skill\s*544$/,
  ]);
  expect(sent.map((body) => [body.kind, body.operation])).toEqual([
    ["tools", "catalog"],
  ]);
});

test("rows, bar and folds show what fills the window", async ({ page }) => {
  const { sheet, sent, release } = await open(page);
  release();
  const row = (name) => sheet.locator(".context-rows > li", { hasText: name });
  await expect(row("Tool results")).toContainText("41.2k");
  await expect(row("Tool results")).toContainText("20.6%");
  await expect(row("Instructions")).toContainText("AGENTS.md");
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
  const tools = sheet.getByRole("button", { name: /^Tools 4/ });
  await expect(tools).toHaveAttribute("aria-expanded", "false");
  await tools.click();
  await expect(tools).toHaveAttribute("aria-expanded", "true");
  await expect(sheet.getByText("apply_patch")).toBeVisible();
  await sheet.getByRole("button", { name: /^MCP tools 2/ }).click();
  // Each server's tool by name, its sizes adding up to the row's 1.6k.
  await expect(
    sheet.locator(".context-rows > li", { hasText: "MCP tools" }).locator("li"),
  ).toHaveText([/^github · create_issue\s*975$/, /^github · get_file\s*625$/]);
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

test("a reply streaming in since the request counts as Replies", async ({
  page,
}) => {
  const { sheet } = await open(page, PARTS.tokens + 2500);
  await expect(sheet.getByText("60% left", { exact: true })).toBeVisible();
  await expect(sheet.getByText("80k of 200k tokens")).toBeVisible();
  const replies = sheet.locator(".context-rows > li", { hasText: "Replies" });
  await expect(replies).toContainText("21.4k");
  await expect(replies).toContainText("10.7%");
  // The bar's parts add up to the headline's total.
  const used = await sheet.evaluate((node) =>
    [...node.querySelectorAll(".context-segment")].reduce(
      (sum, segment) =>
        sum + Number.parseFloat(segment.style.getPropertyValue("--share")),
      0,
    ),
  );
  expect(used).toBeCloseTo(40, 6);
});

test("with automatic compaction off there is no mark and nothing kept", async ({
  page,
}) => {
  const { sheet } = await open(page, PARTS.tokens, {
    ...PARTS,
    compacts_at: 0,
  });
  await expect(
    sheet.getByRole("img", {
      name: "38.8% of the context window is used, 61% left",
    }),
  ).toBeVisible();
  await expect(sheet.getByText("no automatic compaction")).toBeVisible();
  await expect(sheet.locator(".context-mark")).toHaveCount(0);
  await expect(sheet.getByText("Kept for the summary")).toHaveCount(0);
  await expect(sheet.getByText("Free until it compacts")).toHaveCount(0);
  const free = sheet.locator(".context-rows > li", { hasText: /^Free/ });
  await expect(free).toContainText("122.5k");
  await expect(free).toContainText("61.3%");
  // The used parts and what is free fill the window.
  const used = await sheet.evaluate((node) =>
    [...node.querySelectorAll(".context-segment")].reduce(
      (sum, segment) =>
        sum + Number.parseFloat(segment.style.getPropertyValue("--share")),
      0,
    ),
  );
  expect(used).toBeCloseTo(38.75, 6);
});

test("on a phone every row fits on its line", async ({ page }) => {
  const { sheet, release } = await open(page);
  release();
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(sheet.getByText("Tool results")).toBeVisible();
  await sheet.getByRole("button", { name: /^Tools 4/ }).click();
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
