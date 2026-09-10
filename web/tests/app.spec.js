import { test, expect } from "./fixtures.js";
import { readFile, writeFile } from "node:fs/promises";

test.use({ paired: false });

test("native host: mobile decisions, safe rendering, offline shell and private caching", async ({
  page,
  context,
  host: fixture,
  request,
}, testInfo) => {
  const errors = [];
  const external = [];
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("request", (request) => {
    if (!request.url().startsWith(fixture.origin)) external.push(request.url());
  });
  await page.goto("/");
  await page.getByLabel("Single-use pairing code").fill(fixture.code);
  await page
    .getByRole("button", { name: "Connect device", exact: true })
    .click();
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();

  await page
    .getByRole("complementary", { name: "Projects and sessions" })
    .getByRole("button", { name: "New conversation", exact: true })
    .click();
  await page.getByLabel("Directory on the host").fill(fixture.project);
  await page
    .getByRole("button", { name: "Start conversation", exact: true })
    .click();
  await expect(page.locator(".status")).toHaveText("idle");
  const model = page.getByRole("button", {
    name: "Model and effort",
    exact: true,
  });
  await expect(model).toHaveText("mock/model-a:floor:high");
  await page.getByLabel("Message or guidance").fill("Keep this unsent draft");
  await model.click();
  await expect(
    page.getByRole("combobox", { name: "Model", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("button", { name: "Send", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("button", { name: "Send guidance", exact: true }),
  ).toHaveCount(0);
  await page.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(model).toHaveText("mock/model-a:floor:high");
  await model.click();
  await page
    .getByRole("combobox", { name: "Model", exact: true })
    .selectOption({ label: "mock/model-b" });
  await page.getByLabel("Effort", { exact: true }).selectOption("high");
  await page.getByLabel("Variant", { exact: true }).selectOption("floor");
  await expect(model).toHaveText("mock/model-a:floor:high");
  await page.getByRole("button", { name: "Apply", exact: true }).click();
  await expect(model).toHaveText("mock/model-b:floor:high");
  await model.click();
  await expect(page.getByLabel("Model", { exact: true })).toBeEnabled();
  await page.getByLabel("Variant", { exact: true }).selectOption("default");
  await page.getByRole("button", { name: "Apply", exact: true }).click();
  await expect(model).toHaveText("mock/model-b:high");
  const textbox = await page.getByLabel("Message or guidance").boundingBox();
  const selector = await model.boundingBox();
  expect(selector.y).toBeGreaterThanOrEqual(textbox.y + textbox.height);
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "Keep this unsent draft",
  );
  await page.getByRole("button", { name: "Raw context", exact: true }).click();
  await expect(page.getByRole("dialog", { name: "Raw context" })).toContainText(
    "not sent yet",
  );
  await expect(page.getByRole("dialog").locator("pre").last()).toContainText(
    '"tools"',
  );
  await page
    .getByRole("button", { name: "Close raw context", exact: true })
    .click();
  await page.getByLabel("Message or guidance").fill("Show a researched answer");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.locator(".status")).toHaveText("idle");
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  await expect(page.locator(".status")).toHaveText("idle");
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  const reply = page
    .locator(".message")
    .filter({ has: page.getByRole("heading", { name: "Verified response" }) });
  const requestBox = await page.locator(".message.user").first().boundingBox();
  const responseBox = await reply.boundingBox();
  expect(Math.abs(requestBox.x - responseBox.x)).toBeLessThan(2);
  expect(Math.abs(requestBox.width - responseBox.width)).toBeLessThan(2);
  expect(
    Math.abs(
      requestBox.x + requestBox.width - responseBox.x - responseBox.width,
    ),
  ).toBeLessThan(2);
  await expect(page.getByRole("button", { name: /^Reply to:/ })).toHaveCount(0);
  await reply.getByLabel("Message menu", { exact: true }).click();
  await reply
    .getByRole("menuitem", { name: "HTTP request/response", exact: true })
    .click();
  await expect(page.getByRole("dialog").locator("pre").last()).toContainText(
    '"tools"',
  );
  await page.getByRole("tab", { name: "Response", exact: true }).click();
  await expect(page.getByRole("dialog").locator("pre").last()).toContainText(
    "[DONE]",
  );
  await page
    .getByRole("button", { name: "Close http request/response", exact: true })
    .click();
  await expect(reply.locator("header button[aria-label^='Copy']")).toHaveCount(
    0,
  );
  const code = reply.locator(".code-block");
  await expect(code).toHaveCount(1);
  await code.hover();
  await code.getByRole("button", { name: "Copy code", exact: true }).click();
  await expect(
    reply.getByRole("button", { name: "Copied!", exact: true }),
  ).toBeVisible();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(
    "print('hello')\n",
  );
  await expect(reply.locator("time")).toHaveAttribute("datetime", /T/);
  await reply.getByLabel("Message menu", { exact: true }).click();
  await reply
    .getByRole("menuitem", { name: "Statistics", exact: true })
    .click();
  await expect(
    page.getByRole("dialog", { name: "Message statistics" }),
  ).toContainText("mock/model-b:high");
  await expect(page.getByRole("dialog")).toContainText("TTFT");
  await expect(page.getByRole("dialog")).toContainText("tok/s");
  await page
    .getByRole("button", { name: "Close message statistics", exact: true })
    .click();
  const conversationMenu = page
    .locator(".conversation-head")
    .getByLabel("Conversation menu", { exact: true });
  await conversationMenu.click();
  await page
    .locator(".conversation-head")
    .getByRole("menuitem", { name: "Rename", exact: true })
    .click();
  await page.getByLabel("Conversation name").fill("UI refactor proof");
  await page.getByRole("button", { name: "Save name", exact: true }).click();
  await expect(page.locator(".conversation-head h1")).toHaveText(
    "UI refactor proof",
  );
  await conversationMenu.click();
  await page
    .locator(".conversation-head")
    .getByRole("menuitem", { name: "Statistics", exact: true })
    .click();
  await expect(
    page.getByRole("dialog", { name: "Conversation statistics" }),
  ).toContainText("Tool calls");
  await expect(page.getByRole("dialog")).toContainText("Model calls");
  await page
    .getByRole("button", { name: "Close conversation statistics", exact: true })
    .click();
  await expect(page.locator(".katex").first()).toBeVisible();
  await expect(page.locator(".hljs-built_in")).toContainText("print");
  await expect(page.locator(".markdown")).toContainText(["Prices $5 and $10."]);
  expect(await page.locator('a[href^="javascript:"]').count()).toBe(0);
  expect(await page.locator('img[src^="https:"]').count()).toBe(0);
  await page.screenshot({
    path: testInfo.outputPath("desktop.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await page
    .getByRole("button", { name: "Open sessions", exact: true })
    .click();
  await expect(page.getByRole("navigation")).toBeVisible();
  await page
    .getByRole("button", { name: "Close sessions", exact: true })
    .click();
  await page.getByLabel("Message or guidance").fill("request approval");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "Needs your decision" }),
  ).toBeVisible();
  await expect(page.locator(".decision")).toContainText("browser-proof.txt");
  await expect(page.getByLabel("Response", { exact: true })).toHaveValue("n");
  await page.getByLabel("Response", { exact: true }).selectOption("y");
  await page
    .getByRole("button", { name: "Send response", exact: true })
    .click();
  await expect(
    page.getByRole("heading", { name: "Needs your decision" }),
  ).toHaveCount(0);
  await expect
    .poll(() =>
      readFile(`${fixture.project}/browser-proof.txt`, "utf8").catch(
        (error) => {
          if (error.code === "ENOENT") return null;
          throw error;
        },
      ),
    )
    .toBe("approved from browser");
  await expect(page.locator(".status")).toHaveText("idle");
  const toolResult = page.locator(".message.tool").last();
  await toolResult.getByRole("button", { name: /^write_file/ }).click();
  await toolResult
    .getByRole("button", { name: "Tool input/output", exact: true })
    .click();
  await expect(
    page.getByRole("dialog", { name: "Tool input/output" }),
  ).toContainText("browser-proof.txt");
  await expect(page.getByRole("dialog")).toContainText("approved from browser");
  await page.getByRole("tab", { name: "Response", exact: true }).click();
  await expect(page.getByRole("dialog").locator("pre")).not.toHaveText("");
  await expect(
    page.getByRole("dialog").getByRole("button", { name: /Copy/ }),
  ).toHaveCount(0);
  await page
    .getByRole("button", { name: "Close tool input/output", exact: true })
    .click();
  await page.screenshot({
    path: testInfo.outputPath("mobile.png"),
    fullPage: true,
  });
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  await expect(page.locator(".status")).toHaveText("idle");
  // The native host accepts the send while its HTTP acknowledgement is held.
  // Editing the next draft in that interval must not erase the new text.
  let release, acknowledged;
  const held = new Promise((resolve) => (release = resolve));
  const reached = new Promise((resolve) => (acknowledged = resolve));
  await page.route("**/api/command", async (route) => {
    if (route.request().postDataJSON()?.text !== "Delayed acknowledgement")
      return route.continue();
    acknowledged();
    await held;
    const response = await route.fetch();
    await route.fulfill({ response });
  });
  await page.getByLabel("Message or guidance").fill("Delayed acknowledgement");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await reached;
  await expect(
    page
      .locator(".message.user")
      .filter({ hasText: "Delayed acknowledgement" }),
  ).toBeVisible();
  await expect(page.getByLabel("Message or guidance")).toHaveValue("");
  await page.getByLabel("Message or guidance").fill("New text while sending");
  release();
  await expect(
    page.getByRole("button", { name: "Send", exact: true }),
  ).toBeEnabled();
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "New text while sending",
  );
  await page.unroute("**/api/command");
  await expect(
    page.getByText("Delayed acknowledgement", { exact: true }),
  ).toBeVisible();
  await expect(page.locator(".status")).toHaveText("idle");
  await page.getByLabel("Message or guidance").fill("");
  await page.locator('input[type="file"]').setInputFiles({
    name: "one-pixel.png",
    mimeType: "image/png",
    buffer: Buffer.from(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a5WQAAAAASUVORK5CYII=",
      "base64",
    ),
  });
  await expect(page.getByAltText("one-pixel.png")).toBeVisible();
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(
    page.locator(".message").getByAltText("one-pixel.png"),
  ).toBeVisible();
  await expect(page.locator(".status")).toHaveText("idle");
  await page.locator('input[type="file"]').setInputFiles({
    name: "opaque.bin",
    mimeType: "application/octet-stream",
    buffer: Buffer.from([0, 255, 128, 0]),
  });
  await expect(page.locator(".composer .file-chip")).toContainText(
    "opaque.bin",
  );
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(
    page.locator(".message").getByRole("link", { name: /opaque.bin/ }),
  ).toBeVisible();
  await expect(page.locator(".status")).toHaveText("idle");
  await page.getByLabel("Message or guidance").fill("/help");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.getByRole("dialog")).toContainText("/model");
  await page
    .getByRole("button", { name: "Close full content", exact: true })
    .click();
  await page.getByLabel("Message or guidance").fill("Unsent private draft");
  await page.evaluate(async () => {
    await navigator.serviceWorker.ready;
  });
  await context.setOffline(true);
  await expect(
    page.getByRole("button", { name: "Send", exact: true }),
  ).toBeDisabled();
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "Unsent private draft",
  );
  await context.setOffline(false);
  await expect(
    page.getByRole("button", { name: "Send", exact: true }),
  ).toBeEnabled();
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "Unsent private draft",
  );
  const cached = await page.evaluate(async () =>
    (
      await Promise.all(
        (await caches.keys()).map(async (name) =>
          (await (await caches.open(name)).keys()).map(
            (request) => request.url,
          ),
        ),
      )
    ).flat(),
  );
  expect(cached.length).toBeGreaterThan(4);
  expect(cached.some((url) => url.includes("/api/"))).toBe(false);
  expect(errors).toEqual([]);
  expect(external).toEqual([]);

  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await expect(page.getByLabel("Appearance")).toHaveValue("system");
  await page.emulateMedia({ colorScheme: "dark" });
  await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
  await page.emulateMedia({ colorScheme: "light" });
  await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
  await expect(page.locator('meta[name="theme-color"]')).toHaveAttribute(
    "content",
    "#ffffff",
  );
  await page.getByLabel("Display size", { exact: true }).fill("110");
  await page.getByLabel("Text size", { exact: true }).fill("125");
  await expect(page.locator("html")).toHaveCSS("--display-scale", "1.1");
  await expect(page.locator("html")).toHaveCSS("--text-scale", "1.25");
  await page.getByLabel("Text size", { exact: true }).fill("300");
  await expect(page.locator("html")).toHaveCSS("--text-scale", "3");
  await page.getByLabel("Display size", { exact: true }).fill("200");
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  await page.getByLabel("Display size", { exact: true }).fill("110");
  await page.getByLabel("Text size", { exact: true }).fill("125");
  await page
    .getByRole("button", { name: "Advanced configuration", exact: true })
    .click();
  await page.getByLabel("Find a setting").fill("UAGENT_MAX_STEPS");
  await page.getByLabel("UAGENT_MAX_STEPS", { exact: false }).fill("23");
  await page
    .locator(".config-row")
    .getByRole("button", { name: "Apply", exact: true })
    .click();
  await expect(page.locator(".configuration")).toContainText(
    "active at the next user turn",
  );
  await page.getByRole("button", { name: "← Back", exact: true }).click();
  await page.getByLabel("Appearance").selectOption("light");
  await page.emulateMedia({ colorScheme: "dark" });
  await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
  await page
    .getByRole("button", { name: "Close settings", exact: true })
    .click();
  await page.setViewportSize({ width: 844, height: 390 });
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  await page.screenshot({
    path: testInfo.outputPath("light-landscape.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 1280, height: 900 });
  // Navigation now restores the reader's position. Read the existing tail
  // before testing only the other conversation's offline completion badge.
  const latest = page.getByRole("button", { name: "Jump to latest" });
  if (await latest.isVisible()) await latest.click();
  await expect(latest).toHaveCount(0);
  const originalHash = await page.evaluate(() => location.hash);
  await page
    .getByRole("complementary", { name: "Projects and sessions" })
    .getByRole("button", { name: "New conversation", exact: true })
    .click();
  await page.getByLabel("Directory on the host").fill(fixture.project);
  await page
    .getByRole("button", { name: "Start conversation", exact: true })
    .click();
  await expect(page.locator(".status")).toHaveText("idle");
  await model.click();
  await page
    .getByLabel("Model", { exact: true })
    .selectOption({ label: "mock/model-b" });
  await page.getByRole("button", { name: "Apply", exact: true }).click();
  await expect(model).toHaveText(/mock\/model-b/);
  const secondHash = await page.evaluate(() => location.hash);
  await page.getByLabel("Message or guidance").fill("Unread completion probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "Send guidance", exact: true }),
  ).toBeVisible();
  await page.evaluate((hash) => {
    location.hash = hash;
  }, originalHash);
  await expect(page.locator(".conversation-head h1")).toHaveText(
    "UI refactor proof",
  );
  await context.setOffline(true);
  await expect
    .poll(async () => {
      const response = await request.get(
        `/api/sessions/${secondHash.split("=")[1]}`,
        {
          headers: {
            Cookie: (await context.cookies())
              .map(({ name, value }) => `${name}=${value}`)
              .join("; "),
          },
        },
      );
      return (await response.json()).metadata.status;
    })
    .toBe("idle");
  await context.setOffline(false);
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await expect(page.getByLabel("Unread messages", { exact: true })).toHaveCount(
    1,
  );
  await page.evaluate((hash) => {
    location.hash = hash;
  }, secondHash);
  await expect(page.locator(".conversation-head h1")).toHaveText(
    "Unread completion probe",
  );
  await expect(page.getByLabel("Unread messages", { exact: true })).toHaveCount(
    0,
  );
  await expect(page.locator(".error-banner")).toHaveCount(0);
  await page
    .getByLabel("Message or guidance")
    .fill("Background activity probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.locator(".decision")).toContainText("BROWSER_ACTIVITY");
  await page.getByLabel("Response", { exact: true }).selectOption("y");
  await page
    .getByRole("button", { name: "Send response", exact: true })
    .click();
  await expect(
    page.getByRole("button", { name: "Activity", exact: true }),
  ).toContainText("1 command");
  await page.getByRole("button", { name: "Activity", exact: true }).click();
  const activity = page
    .locator(".activity-row")
    .filter({ hasText: "BROWSER_ACTIVITY" });
  await activity.locator("button").first().click();
  await expect(page.getByRole("dialog").locator("pre")).toContainText(
    "BROWSER_ACTIVITY",
  );
  await page
    .getByRole("dialog")
    .getByRole("button", { name: /^Close / })
    .click();
  await activity.getByRole("button", { name: /^Stop / }).click();
  await expect(
    page.getByRole("button", { name: "Activity", exact: true }),
  ).not.toContainText("1 command");
  await page
    .getByRole("button", { name: /Show \d+ completed \/ idle/ })
    .click();
  await expect(activity).toContainText("stopped");
  await expect(page.locator(".status")).toHaveText("idle");
  await conversationMenu.click();
  await page
    .locator(".conversation-head")
    .getByRole("menuitem", { name: "Fork conversation", exact: true })
    .click();
  await expect(page.locator(".conversation-head h1")).toHaveText(
    "Fork of Unread completion probe",
  );
  await expect(page.locator(".status")).toHaveText("idle");
  await expect(model).toHaveText(/mock\/model-b/);
  await expect(page.locator(".message.user").first()).toContainText(
    "Unread completion probe",
  );
  await page.getByRole("button", { name: "Permissions", exact: true }).click();
  await page
    .getByRole("combobox", { name: "Permissions", exact: true })
    .selectOption("yolo");
  await expect(
    page.getByRole("button", { name: "Permissions", exact: true }),
  ).toHaveText("YOLO");
  await page.getByRole("button", { name: "Permissions", exact: true }).click();
  await page
    .getByRole("combobox", { name: "Permissions", exact: true })
    .selectOption("ask");

  await page.evaluate((hash) => {
    location.hash = hash;
  }, originalHash);
  await expect(page.locator(".conversation-head h1")).toHaveText(
    "UI refactor proof",
  );
  await conversationMenu.click();
  await expect(
    page
      .locator(".conversation-head")
      .getByRole("menuitem", { name: "Delete", exact: true }),
  ).toBeDisabled();
  await page
    .locator(".conversation-head")
    .getByRole("menuitem", { name: "Close session", exact: true })
    .click();
  await expect(page.locator(".status")).toHaveText("saved");
  await conversationMenu.click();
  await page
    .locator(".conversation-head")
    .getByRole("menuitem", { name: "Delete", exact: true })
    .click();
  await expect(
    page.getByRole("dialog", { name: "Delete conversation" }),
  ).toContainText("UI refactor proof");
  await page
    .getByRole("button", { name: "Delete permanently", exact: true })
    .click();
  await expect(page.locator(".conversation-head h1")).toHaveText(
    "Your workspace",
  );
  await expect(
    page.getByRole("button", { name: /UI refactor proof/ }),
  ).toHaveCount(0);
  const messages = Array.from({ length: 2000 }, (_, index) => ({
    role: index % 2 ? "assistant" : "user",
    content:
      `Retained message ${index}. ` + "Bounded history rendering. ".repeat(30),
  }));
  const header = {
    format: 3,
    cwd: fixture.project,
    model: "test",
    session_id: "large-history",
    title: "Large retained history",
    turns: 1000,
  };
  const state = {
    messages,
    message_kinds: messages.map((item) => item.role),
    archive: [],
    archive_dropped_segments: 0,
    context_tokens: 0,
    usage: {},
    tool_displays: {},
  };
  await writeFile(
    `${fixture.home}/.uagent/history/large.json`,
    JSON.stringify(header) + "\n" + JSON.stringify(state),
    { mode: 0o600 },
  );
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect(
    page.getByRole("button", { name: /Large retained history/ }),
  ).toBeVisible();
  const started = Date.now();
  await page.getByRole("button", { name: /Large retained history/ }).click();
  await expect(page.locator(".message")).toHaveCount(64);
  const openMs = Date.now() - started;
  expect(openMs).toBeLessThan(3000);
  await writeFile(
    testInfo.outputPath("history-metrics.json"),
    JSON.stringify({ messages: 2000, visible: 64, open_ms: openMs }),
  );
  // Resolve two reads of the same older page in reverse order. It is merged once.
  let releasePage;
  const pageGate = new Promise((resolve) => (releasePage = resolve));
  let pageFinished;
  const pageDone = new Promise((resolve) => (pageFinished = resolve));
  let pageRequests = 0;
  const olderRoute = async (route) => {
    const first = ++pageRequests === 1;
    const response = await route.fetch();
    if (first) await pageGate;
    await route.fulfill({ response });
    if (first) pageFinished();
  };
  await page.route("**/api/sessions/*?before=*", olderRoute);
  await page
    .getByRole("button", { name: "Load older retained messages", exact: true })
    .evaluate((element) => {
      element.click();
      element.click();
    });
  await expect(page.locator(".message")).toHaveCount(128);
  releasePage();
  await pageDone;
  await page.evaluate(
    () =>
      new Promise((resolve) =>
        requestAnimationFrame(() => requestAnimationFrame(resolve)),
      ),
  );
  expect(pageRequests).toBe(2);
  await expect(page.locator(".message")).toHaveCount(128);
  const ids = await page
    .locator(".message")
    .evaluateAll((elements) =>
      elements.map((element) => element.dataset.messageId),
    );
  expect(new Set(ids).size).toBe(ids.length);
  await page.unroute("**/api/sessions/*?before=*", olderRoute);
  for (let index = 0; index < 5; ++index) {
    await page.locator(".transcript").evaluate((element) => {
      element.scrollTop = 0;
    });
    const prior = await page
      .locator(".message")
      .first()
      .getAttribute("data-message-id");
    await page
      .getByRole("button", {
        name: "Load older retained messages",
        exact: true,
      })
      .click();
    await expect(page.locator(".message").first()).not.toHaveAttribute(
      "data-message-id",
      prior,
    );
  }
  expect(await page.locator(".message").count()).toBeLessThanOrEqual(256);
  await page
    .getByRole("button", { name: "Jump to latest", exact: true })
    .click();
  await expect(page.locator(".message")).toHaveCount(64);
  await expect(page.locator(".message").last()).toContainText(
    "Retained message 1999.",
  );
});
