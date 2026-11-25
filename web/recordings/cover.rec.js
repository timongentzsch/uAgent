// docs/images/cover.png, how-it-works.png and remote-execution.png: stills of
// one fixture host, laid out by the pages in recordings/figures. The remote
// host and the model catalogue are answered here; the Chrome still is
// docs/images/browser-sign-in.png, the container's browser pane.
import { mkdir, readFile, realpath, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
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
const figures = { cover: 924, "how-it-works": 700, "remote-execution": 560 };
const models = [
  ["anthropic/claude-sonnet-5-5", "Claude Sonnet 5.5"],
  ["local/qwen3-coder", "Qwen3 Coder"],
  ["openai/gpt-6.1", "GPT-6.1"],
  ["openrouter/deepseek/deepseek-v4", "DeepSeek V4"],
];
const benchmark =
  "python3 benchmarks/compare_agents.py --from-report report.json";

test("cover", async ({ page, host, request, command, browser }, testInfo) => {
  test.setTimeout(180000);
  const stills = testInfo.outputPath("stills");
  await mkdir(stills, { recursive: true });
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
  await page.route("**/api/command", async (route) => {
    const body = route.request().postDataJSON();
    if (body.kind !== "model" || body.operation !== "catalog")
      return route.continue();
    await route.fulfill({
      json: {
        v: 2,
        accepted: true,
        pending: false,
        request_id: body.request_id,
        result: {
          models: models.map(([value, name], index) => ({
            value,
            label: value,
            name,
            active: !index,
            efforts: ["high", "low"],
          })),
        },
      },
    });
  });

  await page.goto(`/#session=${shown.id}`);
  await online(page);
  await expect(page.getByText("The run stayed on gpu-box.")).toBeVisible();
  await page.mouse.move(0, 0);
  await page.evaluate(() => document.fonts.ready);
  await page.screenshot({ path: `${stills}/desk.png`, animations: "disabled" });
  const hosts = await page
    .getByRole("button", { name: "Remote hosts" })
    .boundingBox();
  const add = await page
    .getByRole("button", { name: "Add host" })
    .boundingBox();
  await page.screenshot({
    path: `${stills}/remote.png`,
    clip: {
      x: 0,
      y: hosts.y - 8,
      width: 260,
      height: add.y + add.height + 16 - hosts.y,
    },
  });
  await page
    .getByRole("button", { name: "Model and effort", exact: true })
    .click();
  const picker = page.getByRole("dialog", { name: "Model and effort" });
  await picker.getByRole("combobox", { name: "Model" }).click();
  await expect(picker.getByText("openai/gpt-6.1")).toBeVisible();
  const sheet = await picker.boundingBox();
  await page.screenshot({
    path: `${stills}/models.png`,
    clip: { x: sheet.x, y: sheet.y, width: sheet.width, height: 340 },
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
    path: `${stills}/phone.png`,
    animations: "disabled",
  });
  await panel
    .locator(".xterm")
    .screenshot({ path: `${stills}/terminal.png`, animations: "disabled" });
  await deny.tap();
  await expect(panel.locator(".xterm-rows")).toContainText("was not run");
  await pocket.close();
  await window.close();

  const frame = await browser.newContext({
    viewport: { width: 1280, height: 1000 },
    deviceScaleFactor: 2,
  });
  const sheetPage = await frame.newPage();
  const root = pathToFileURL(`${process.cwd()}/..`).href;
  for (const [name, height] of Object.entries(figures)) {
    const source = await readFile(`recordings/figures/${name}.html`, "utf8");
    const file = `${stills}/${name}.html`;
    await writeFile(file, source.replaceAll("{root}", root));
    await sheetPage.goto(pathToFileURL(file).href);
    await sheetPage.evaluate(() =>
      Promise.all([...document.images].map((image) => image.decode())),
    );
    await sheetPage.screenshot({
      path: `../docs/images/${name}.png`,
      clip: { x: 0, y: 0, width: 1280, height },
    });
  }
  await frame.close();
});
