import {
  test,
  expect,
  online,
  selectProbeModel,
  chooseDetail,
  openRow,
  openMore,
} from "./fixtures.js";
import { readFile, writeFile } from "node:fs/promises";

test("tool catalogue shows its schema size and persists selection", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1200, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Conversation menu" }).last().click();
  await openMore(page);
  await page.getByRole("menuitem", { name: "Tools", exact: true }).click();

  const dialog = page.getByRole("dialog", { name: "Tools", exact: true });
  await expect(dialog.getByText(/serialized schema/)).toBeVisible();
  await expect(dialog.getByText(/estimated.*tokens/i)).toHaveCount(0);

  const profile = dialog.getByRole("combobox", { name: "Tool profile" });
  await profile.selectOption("minimal");
  await expect(profile).toHaveValue("minimal");

  const search = dialog.getByRole("searchbox", { name: "Find a tool" });
  await search.fill("read_path");
  const choice = dialog.locator(".tool-choice");
  await expect(choice).toHaveCount(1);
  await expect(choice.locator("code")).toHaveText("read_path");
  const enabled = choice.getByRole("checkbox");
  await expect(enabled).toBeChecked();
  await enabled.uncheck();
  await expect(enabled).not.toBeChecked();
});

test("a presented file previews inline in the conversation, sandboxed", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Report probe");
  await prompt.press("Enter");
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  await chooseDetail(page, "Full");
  await expect(page.locator(".node .diff").first()).toContainText(
    "PRESENTED_REPORT",
  );
  const file = page.locator(".transcript .node-file");
  await expect(file).toContainText("report.html");
  const frame = file.locator("iframe");
  await expect(frame).toHaveAttribute(
    "sandbox",
    "allow-scripts allow-forms allow-popups",
  );
  await expect(
    page.frameLocator(".node-file iframe").getByText("PRESENTED_REPORT"),
  ).toBeVisible();
  // An opaque origin: the file's scripts can never read this device's session.
  const preview = page.frames().find((item) => item.url().includes("/assets/"));
  expect(
    await preview.evaluate(() => {
      try {
        return document.cookie;
      } catch {
        return "blocked";
      }
    }),
  ).toBe("blocked");
  await expect(file.getByRole("link", { name: "Open" })).toHaveAttribute(
    "target",
    "_blank",
  );
  await expect(file.getByRole("link", { name: "Download" })).toHaveAttribute(
    "href",
    /\?download=1$/,
  );
  await chooseDetail(page, "Minimal");
  await expect(page.locator(".transcript .node-row")).toHaveText([
    /^Worked 2 steps/,
  ]);
  await expect(page.locator(".node .diff")).toHaveCount(0);
  await expect(file).toHaveCount(1);
  await expect(file.locator("iframe")).toBeVisible();
  await chooseDetail(page, "Default");
});

test("a call that names no tool reads as what it called, never as its JSON", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Unknown tool probe");
  await prompt.press("Enter");
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  const row = page.locator(".node.failed > .node-row");
  await expect(row).toContainText("Called Read Paths /etc");
  await expect(row).not.toContainText("{");
  await page.reload();
  await expect(row).toContainText("Called Read Paths /etc");
});

test("a to-do list reads as its progress and stays above the composer", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const todo = page.getByRole("region", { name: "To-dos" });
  await expect(todo).toHaveCount(0);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Todo probe");
  await prompt.press("Enter");
  const call = await openRow(page, "Updated to-dos");
  await expect(call.locator(".node-row")).toContainText("1/3 done");
  await expect(call).toContainText("[pending] Fix the lexer");
  await expect(todo.getByText("To-dos · 1/3 done")).toBeVisible();
  await expect(todo.getByRole("listitem")).toHaveCount(3);
  await expect(todo.locator('[aria-current="step"]')).toHaveText(
    "Fix the lexer",
  );
  await expect(todo.locator('[data-status="done"]')).toHaveCSS(
    "text-decoration-line",
    "line-through",
  );
  await page.reload();
  await expect(todo.getByRole("listitem")).toHaveCount(3);
});

test("a long diff shows whole on its row, live and after a reload", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Long diff probe");
  await prompt.press("Enter");
  const write = await openRow(page, "Wrote long.txt");
  await expect(write.locator(".node-row")).toContainText("+2001 −0");
  const diff = write.locator(".diff");
  await expect(diff.locator("span")).toHaveCount(2001);
  await expect(diff.locator("span").last()).toHaveText("+LAST_DIFF_LINE");
  await page.reload();
  await expect(diff.locator("span")).toHaveCount(2001);
  await expect(diff.locator("span").last()).toHaveText("+LAST_DIFF_LINE");
  await expect(write).not.toContainText("more lines");
});

test("files read as tiles and cards, tool images sit on their row, and every image opens the viewer", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  const pixel = Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==",
    "base64",
  );
  await online(page);
  await page.locator('input[type="file"]').setInputFiles([
    { name: "layout.png", mimeType: "image/png", buffer: pixel },
    {
      name: "spec.pdf",
      mimeType: "application/pdf",
      buffer: Buffer.from("%PDF-1.4"),
    },
  ]);
  const strip = page.locator(".composer .attachments");
  await expect(strip.locator(".file-chip")).toHaveCount(2);
  // Uploaded, not merely listed: Enter does nothing while one is under way.
  await expect(strip.locator("[aria-busy]")).toHaveCount(0);
  expect(await strip.evaluate((node) => getComputedStyle(node).flexWrap)).toBe(
    "nowrap",
  );
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Image probe");
  await prompt.press("Enter");
  const sent = page.locator(".message.user .attachment-list");
  await expect(sent.locator(".attachment-tile")).toHaveCount(1);
  await expect(sent.locator(".attachment-card")).toContainText("PDF");
  const read = await openRow(page, "shot.png");
  await expect(read.locator(".attachment-tile")).toHaveCount(1);
  await expect(
    page.locator(".message").filter({ hasText: "attached on request" }),
  ).toHaveCount(0);
  await read.locator(".attachment-tile").click();
  const viewer = page.getByRole("dialog", { name: "shot.png" });
  await expect(viewer.locator("img")).toBeVisible();
  await viewer.locator("img").click();
  await expect(viewer).toBeVisible();
  const surface = await viewer
    .getByRole("group", { name: "shot.png" })
    .boundingBox();
  await page.mouse.click(surface.x + 4, surface.y + 4);
  await expect(viewer).toHaveCount(0);
});

test("MCP servers show their state and switch on and off", async ({
  page,
  host,
  command,
  request,
}) => {
  const server = `${host.home}/fake_mcp.py`;
  await writeFile(
    server,
    [
      "import json, sys",
      "for line in sys.stdin:",
      "    message = json.loads(line)",
      "    if 'id' not in message: continue",
      "    method = message.get('method')",
      "    result = {'supportedVersions': ['2026-07-28'], 'capabilities': {'tools': {}}} if method == 'server/discover' else {'tools': [{'name': 'echo', 'inputSchema': {'type': 'object'}}]} if method == 'tools/list' else {}",
      "    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': result}), flush=True)",
    ].join("\n"),
  );
  const config = `${host.home}/.mcp.json`;
  await writeFile(
    config,
    JSON.stringify({
      mcpServers: {
        probe: { command: "python3", args: [server] },
        broken: { command: `${host.home}/missing`, required: false },
      },
    }),
  );
  let { session } = await command("create", { cwd: host.project });
  ({ session } = await command("activate", { session_id: session.id }));
  await expect
    .poll(async () => {
      const response = await request.get(`/api/sessions/${session.id}`);
      return (await response.json()).metadata.status;
    })
    .toBe("idle");
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  const settings = page.getByRole("dialog", { name: "Settings", exact: true });
  await settings
    .getByRole("button", { name: "All conversations", exact: true })
    .click();

  const global = settings.getByRole("region", { name: "MCP servers" });
  await expect(global).toContainText("~/.mcp.json");
  const probe = global.getByRole("button", { name: /probe/ });
  await expect(probe).toContainText("1 tool");
  await expect(global.getByRole("button", { name: /broken/ })).toContainText(
    /exited|failed/i,
  );

  await probe.click();
  await global.getByRole("switch", { name: "Enable probe" }).uncheck();
  await expect(probe).toContainText("Disabled");
  expect(
    JSON.parse(await readFile(config, "utf8")).mcpServers.probe.disabled,
  ).toBe(true);
  await global.getByRole("switch", { name: "Enable probe" }).check();
  await expect(probe).toContainText("1 tool");

  await global.getByRole("button", { name: /broken/ }).click();
  await global.getByRole("button", { name: "Retry" }).click();
  await expect(global.getByRole("button", { name: /broken/ })).toContainText(
    /exited|failed/i,
  );

  await settings
    .locator(".settings-nav")
    .getByRole("button", { name: /^This folder/ })
    .click();
  const project = settings.getByRole("region", { name: "MCP servers" });
  await expect(project).toContainText("No servers");
  await expect(project).toContainText("/.mcp.json");
});
