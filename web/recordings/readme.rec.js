// The README's stills of one fixture host: a conversation that ran on a
// remote host, and a terminal and a phone asked to approve the same command.
// The remote host is answered here.
import { mkdir, realpath } from "node:fs/promises";
import {
  expect,
  headerAction,
  online,
  saveHistory,
  test,
  withoutServiceWorker,
} from "../tests/fixtures.js";

const desk = { width: 860, height: 604 };
const phone = { width: 390, height: 760 };
test.use({
  viewport: desk,
  deviceScaleFactor: 2,
  colorScheme: "light",
  video: "off",
  places: true,
});

const remote = "b".repeat(32);
const images = "../docs/images";
const benchmark =
  "python3 benchmarks/compare_agents.py --from-report report.json";

test("readme", async ({ page, host, request, command, browser }) => {
  test.setTimeout(180000);
  const save = (title, cwd, fields = {}) =>
    saveHistory(host, `cover-${crypto.randomUUID()}.json`, {
      id: crypto.randomUUID(),
      title,
      cwd,
      turns: 1,
      messages: [
        { role: "user", content: title },
        { role: "assistant", content: "Done" },
      ],
      ...fields,
    });
  const space = async (name) => {
    const path = `${host.home}/.uagent/spaces/${name}`;
    await mkdir(`${path}/.uagent`, { recursive: true });
    return realpath(path);
  };
  await save("Draft the 1.12 changelog", await space("Release 1.12"));
  const title = "Verify the footprint numbers";
  await save(title, host.project, {
    messages: [
      {
        role: "user",
        content:
          "Check µAgent's idle memory on gpu-box against the raw samples.",
      },
      {
        role: "assistant",
        content: "",
        tool_calls: [
          {
            id: "read",
            type: "function",
            function: {
              name: "read_path",
              arguments: '{"path":"benchmarks/README.md"}',
            },
          },
          {
            id: "run",
            type: "function",
            function: {
              name: "run",
              arguments: JSON.stringify({ command: benchmark }),
            },
          },
        ],
      },
      { role: "tool", tool_call_id: "read", content: "# Benchmarks" },
      { role: "tool", tool_call_id: "run", content: "27.4 MiB\n[exit code 0]" },
      {
        role: "assistant",
        content:
          "**The published median holds: 27.4 MiB idle.** Five trials on Linux ARM64, recomputed from the raw samples.\n\n| CLI | Idle memory | CPU per reply |\n| --- | ---: | ---: |\n| µAgent 1.12.0 | 27.4 MiB | 19 ms |\n| Claude Code 2.1.331 | 217.9 MiB | 374 ms |\n\nThe run stayed on gpu-box.",
      },
    ],
    kinds: ["user", "assistant", "tool_result", "tool_result", "assistant"],
    display: {
      ids: [1, 2, 3, 4, 5],
      next: 6,
      facts: {
        "t-read": {
          name: "read_path",
          call_id: "read",
          state: "ok",
          view: { verb: ["Reading", "Read"], target: "benchmarks/README.md" },
          result: { word: "212 lines" },
        },
        "t-run": {
          name: "run",
          call_id: "run",
          state: "ok",
          view: { verb: ["Running", "Ran"], target: benchmark },
          result: { word: "exit 0 · 1.4s" },
        },
      },
    },
  });
  let shown;
  await expect
    .poll(async () => {
      const data = await (await request.get("/api/sessions?refresh=1")).json();
      shown = data.sessions.find((item) => item.title === title);
      return Boolean(shown);
    })
    .toBe(true);

  await withoutServiceWorker(page);
  await page.route("**/api/hosts", (route) =>
    route.fulfill({
      json: {
        hosts: [
          { id: remote, address: "gpu-box", phase: "connected", error: "" },
        ],
      },
    }),
  );
  // The shown conversation and one more belong to the remote host.
  await page.route(/\/api\/sessions(?:\?.*)?$/, async (route) => {
    const catalogue = await (await route.fetch()).json();
    const at = { host: remote, cwd: "/srv/uAgent" };
    Object.assign(
      catalogue.sessions.find((item) => item.id === shown.id),
      at,
    );
    catalogue.sessions.push({
      ...at,
      id: "a".repeat(32),
      title: "Run the TSan suite",
      updated: Date.now() - 3600e3,
      turns: 3,
      status: "saved",
      generation: "",
    });
    await route.fulfill({ json: catalogue });
  });
  // The fixture's provider is named mock; the conversation shows a real one.
  await page.route(new RegExp(`/api/sessions/${shown.id}$`), async (route) => {
    const response = await route.fetch();
    const snapshot = await response.json();
    if (snapshot.state)
      snapshot.state.route = "anthropic/claude-sonnet-5-5:high";
    await route.fulfill({ response, json: snapshot });
  });
  await page.goto(`/#session=${shown.id}`);
  await online(page);
  await expect(page.getByText("The run stayed on gpu-box.")).toBeVisible();
  await page.mouse.move(0, 0);
  await page.evaluate(() => document.fonts.ready);
  await page.screenshot({
    path: `${images}/remote-conversation.png`,
    animations: "disabled",
  });

  // A terminal client starts a turn; the phone is asked to approve it.
  const state = await page.context().storageState();
  const dark = { colorScheme: "dark", storageState: state };
  const window = await browser.newContext({
    ...dark,
    viewport: { width: 720, height: 560 },
    deviceScaleFactor: 2,
  });
  const terminal = await window.newPage();
  const { session } = await command("create", { cwd: host.project });
  await command("activate", { session_id: session.id });
  await terminal.goto(new URL(`/#session=${session.id}`, page.url()).href);
  await online(terminal);
  await headerAction(terminal, "Terminal");
  const panel = terminal.locator(".terminal-tabpanel:not([hidden])");
  await expect(panel.locator(".xterm-rows")).toContainText(/\S/);
  const edge = await terminal
    .locator(".terminal-dock > .resize-handle")
    .boundingBox();
  await terminal.mouse.move(edge.x + edge.width / 2, edge.y + edge.height / 2);
  await terminal.mouse.down();
  await terminal.mouse.move(edge.x + edge.width / 2, 60, { steps: 6 });
  await terminal.mouse.up();
  await panel.locator(".xterm").click();
  await terminal.keyboard.type(
    `PS1='$ '; export UAGENT_MODEL=mock/main:high; alias uagent=${process.env.UAGENT_TEST_BINARY}; clear\n`,
  );
  await terminal.keyboard.type("uagent\n");
  await expect(panel.locator(".xterm-rows")).toContainText("Ready");
  await terminal.keyboard.type("Check the idle memory benchmark\n");
  await expect(panel.locator(".xterm-rows")).toContainText("Allow run?");

  const pocket = await browser.newContext({
    ...dark,
    viewport: phone,
    deviceScaleFactor: 3,
    hasTouch: true,
    isMobile: true,
  });
  const hand = await pocket.newPage();
  await hand.goto(new URL("/", page.url()).href);
  await hand
    .getByRole("button", { name: /Check the idle/ })
    .first()
    .tap();
  const deny = hand.getByRole("button", { name: "Deny", exact: true });
  await expect(deny).toBeVisible();
  await expect(hand.locator(".conversation-head")).toContainText("Check the");
  await hand.evaluate(() => document.fonts.ready);
  await hand.screenshot({
    path: `${images}/phone-approval.png`,
    animations: "disabled",
  });
  await panel.locator(".xterm").screenshot({
    path: `${images}/terminal-approval.png`,
    animations: "disabled",
  });
  await deny.tap();
  await expect(panel.locator(".xterm-rows")).toContainText("was not run");
  await pocket.close();
  await window.close();
});
