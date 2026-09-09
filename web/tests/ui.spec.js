import { test, expect } from "@playwright/test";
import { readFile, writeFile } from "node:fs/promises";

test("compact surfaces stay anchored, accessible and usable while loading", async ({
  page,
  context,
}, testInfo) => {
  const fixture = JSON.parse(await readFile("test-results/host.json", "utf8"));
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  // A valid empty worker keeps cold lazy-module tests independent of precaching.
  await page.route("**/sw.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "" }),
  );
  try {
    await context.addCookies(
      JSON.parse(await readFile("test-results/device-state.json", "utf8"))
        .cookies,
    );
  } catch {
    /* First test may pair directly. */
  }
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto("/");
  await expect(
    page
      .getByText("Connected", { exact: true })
      .or(page.getByLabel("Single-use pairing code")),
  ).toBeVisible();
  if (await page.getByLabel("Single-use pairing code").isVisible()) {
    await page.getByLabel("Single-use pairing code").fill(fixture.code);
    await page
      .getByRole("button", { name: "Connect device", exact: true })
      .click();
  }
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await context.storageState({ path: "test-results/device-state.json" });
  await page
    .getByRole("complementary")
    .getByRole("button", { name: "New conversation", exact: true })
    .click();
  await page.getByLabel("Directory on the host").fill(fixture.project);
  await page
    .getByRole("button", { name: "Start conversation", exact: true })
    .click();
  await expect(page.locator(".status")).toHaveText("idle");
  const prompt = page.getByLabel("Message or guidance");
  const model = page.getByRole("button", {
    name: "Model and effort",
    exact: true,
  });
  await expect(model).toBeVisible();
  const metrics = {};
  const measure = async (name) => {
    await expect
      .poll(() =>
        page
          .locator(".composer")
          .evaluate(
            (element) =>
              element.getBoundingClientRect().bottom <= innerHeight + 1,
          ),
      )
      .toBe(true);
    metrics[name] = await page.evaluate(() => {
      const rect = (selector) => {
        const { x, y, width, height } = document
          .querySelector(selector)
          .getBoundingClientRect();
        return { x, y, width, height };
      };
      return {
        viewport: { width: innerWidth, height: innerHeight },
        header: rect(".conversation-head"),
        transcript: rect(".transcript"),
        composer: rect(".composer"),
        horizontalOverflow: document.documentElement.scrollWidth > innerWidth,
      };
    });
    expect(metrics[name].horizontalOverflow).toBe(false);
    expect(
      metrics[name].composer.y + metrics[name].composer.height,
    ).toBeLessThanOrEqual(metrics[name].viewport.height + 1);
    if (await page.locator(".message.user").count()) {
      const user = await page.locator(".message.user").first().boundingBox();
      const response = await page
        .locator(".message.response")
        .first()
        .boundingBox();
      expect(Math.abs(user.x - response.x)).toBeLessThan(2);
      expect(Math.abs(user.width - response.width)).toBeLessThan(2);
      await expect(page.locator(".message.user").first()).toHaveCSS(
        "text-align",
        "left",
      );
      await expect(
        page.getByRole("button", { name: /^Reply to:/ }),
      ).toHaveCount(0);
    }
    await page.screenshot({
      path: `test-results/${testInfo.project.name}-${name}.png`,
    });
  };
  await measure("desktop-compact");
  expect(metrics["desktop-compact"].transcript.height).toBeGreaterThanOrEqual(
    714,
  );
  await prompt.fill("Keep this draft through popups");
  await model.click();
  const picker = page.getByRole("dialog", {
    name: "Model and effort",
    exact: true,
  });
  await expect(
    picker.getByRole("combobox", { name: "Model", exact: true }),
  ).toBeFocused();
  const anchor = await model.boundingBox(),
    panel = await picker.boundingBox();
  expect(Math.abs(anchor.x - panel.x)).toBeLessThan(2);
  expect(Math.abs(anchor.y - panel.y - panel.height - 8)).toBeLessThan(2);
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-model.png`,
  });
  await page.keyboard.press("Escape");
  await expect(picker).toHaveCount(0);
  await expect(model).toBeFocused();
  await model.click();
  await expect(picker).toBeVisible();
  await prompt.click();
  await expect(picker).toHaveCount(0);
  await expect(prompt).toHaveValue("Keep this draft through popups");

  let releaseSettings;
  const settingsGate = new Promise((resolve) => (releaseSettings = resolve));
  await page.route("**/assets/settings-*.js", async (route) => {
    await settingsGate;
    await route.continue();
  });
  const settingsButton = page.getByRole("button", {
    name: "Settings",
    exact: true,
  });
  await settingsButton.focus();
  await page.keyboard.press("Enter");
  const settings = page.getByRole("dialog", { name: "Settings", exact: true });
  await expect(settings).toBeVisible();
  await expect(settings.getByRole("status")).toHaveAttribute(
    "aria-busy",
    "true",
  );
  await expect(
    settings.getByRole("button", { name: "Close settings" }),
  ).toBeFocused();
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-settings-loading.png`,
  });
  const settingsLoadingBox = await settings.boundingBox();
  releaseSettings();
  await expect(settings.getByLabel("Appearance")).toBeVisible();
  await expect(settings.getByLabel("Default permissions")).toBeVisible();
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-settings.png`,
  });
  expect(
    Math.abs((await settings.boundingBox()).height - settingsLoadingBox.height),
  ).toBeLessThan(6);
  const appearance = await settings.getByLabel("Appearance").boundingBox();
  const displayField = await settings
    .getByLabel("Display size", { exact: true })
    .locator("..")
    .boundingBox();
  expect(
    displayField.y - appearance.y - appearance.height,
  ).toBeGreaterThanOrEqual(12);
  await settings.getByRole("button", { name: "Close settings" }).click();
  await expect(settingsButton).toBeFocused();

  await model.click();
  await picker
    .getByRole("combobox", { name: "Model", exact: true })
    .selectOption({ label: "mock/model-b" });
  await picker.getByRole("button", { name: "Apply", exact: true }).click();
  await prompt.fill("Compact layout proof");
  await prompt.evaluate((element) =>
    element.setSelectionRange(element.value.length, element.value.length),
  );
  await prompt.press("Shift+Enter");
  await expect(prompt).toHaveValue("Compact layout proof\n");
  for (const keyboard of [
    { key: "Enter", isComposing: true },
    { key: "Enter", keyCode: 229 },
    { key: "Enter", repeat: true },
  ]) {
    await prompt.dispatchEvent("keydown", keyboard);
    await expect(prompt).toHaveValue("Compact layout proof\n");
    await expect(page.locator(".message.user")).toHaveCount(0);
  }
  await prompt.press("Enter");
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  await expect(page.locator(".status")).toHaveText("idle");
  await expect(
    page.getByRole("button", { name: "Raw context", exact: true }),
  ).toContainText("/1.3M · 99% left");
  const reply = page.locator(".message.response").last();
  // Tailnet HTTP has no Clipboard API; a user click must still copy.
  await page.evaluate(() =>
    Object.defineProperty(navigator, "clipboard", { value: undefined }),
  );
  await reply.getByRole("button", { name: "Copy", exact: true }).click();
  await expect(
    reply.getByRole("button", { name: "Copied", exact: true }),
  ).toBeVisible();
  const thinking = reply.locator(".thinking");
  await thinking.locator("summary").click();
  await expect(thinking).toContainText("I checked the supplied evidence.");
  const thinkingColor = await thinking.evaluate(
    (element) => getComputedStyle(element).color,
  );
  await expect(thinking.locator(".markdown, .plain")).toHaveCSS(
    "color",
    thinkingColor,
  );
  expect(thinkingColor).not.toBe(
    await reply.evaluate((element) => getComputedStyle(element).color),
  );
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-thinking-grey.png`,
  });
  await page.emulateMedia({ colorScheme: "dark" });
  await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
  const darkThinking = await thinking.evaluate(
    (element) => getComputedStyle(element).color,
  );
  expect(darkThinking).not.toBe(thinkingColor);
  await expect(thinking.locator(".markdown, .plain")).toHaveCSS(
    "color",
    darkThinking,
  );
  expect(darkThinking).not.toBe(
    await reply.evaluate((element) => getComputedStyle(element).color),
  );
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-thinking-grey-dark.png`,
  });
  await page.emulateMedia({ colorScheme: "light" });

  await reply
    .getByRole("button", { name: "Message menu", exact: true })
    .click();
  const rawAction = page.getByRole("menuitem", {
    name: "HTTP request/response",
    exact: true,
  });
  await expect(rawAction).toBeVisible();
  expect(
    await rawAction.evaluate((element) => {
      const r = element.getBoundingClientRect();
      return element.contains(
        document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2),
      );
    }),
  ).toBe(true);
  let releaseRaw;
  const rawGate = new Promise((resolve) => (releaseRaw = resolve));
  await page.route("**/assets/raw-*.js", async (route) => {
    await rawGate;
    await route.continue();
  });
  await rawAction.click();
  const raw = page.getByRole("dialog", {
    name: "HTTP request/response",
    exact: true,
  });
  await expect(raw.getByRole("status")).toHaveAttribute("aria-busy", "true");
  await expect(
    raw.getByRole("button", { name: "Download", exact: true }),
  ).toBeDisabled();
  const rawLoadingBox = await raw.boundingBox();
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-raw-loading.png`,
  });
  releaseRaw();
  await expect(raw.getByRole("tabpanel").locator("pre").last()).toContainText(
    '\n  "',
  );
  expect(
    Math.abs((await raw.boundingBox()).height - rawLoadingBox.height),
  ).toBeLessThan(3);
  await expect(raw.getByRole("checkbox")).toHaveCount(0);
  await expect(
    raw.getByRole("button", { name: "Readable", exact: true }),
  ).toHaveAttribute("aria-pressed", "true");
  const readableRequest = await raw.locator(".raw-body > pre").textContent();
  await raw.getByRole("button", { name: "Source", exact: true }).click();
  const sourceRequest = await raw.locator(".raw-body > pre").textContent();
  expect(sourceRequest).toContain("\\n");
  expect(readableRequest).not.toBe(sourceRequest);
  await raw.getByRole("button", { name: "Readable", exact: true }).click();
  await expect(raw.locator(".raw-body > pre")).toHaveText(readableRequest, {
    useInnerText: false,
  });
  const requestDownload = page.waitForEvent("download");
  await raw.getByRole("button", { name: "Download", exact: true }).click();
  expect(await readFile(await (await requestDownload).path(), "utf8")).toBe(
    sourceRequest,
  );
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-readable-request.png`,
  });

  await raw.getByRole("tab", { name: "Response", exact: true }).click();
  await expect(raw.getByRole("tabpanel")).toContainText("[DONE]");
  await expect(
    raw.getByRole("button", { name: "Events", exact: true }),
  ).toHaveAttribute("aria-pressed", "true");
  await expect(raw.locator(".raw-body > pre")).not.toContainText("data:");
  await raw.getByRole("button", { name: "Source", exact: true }).click();
  await expect(raw.locator(".raw-body > pre")).toContainText("data: [DONE]");
  await raw.getByRole("button", { name: "Events", exact: true }).click();
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-raw.png`,
  });
  await raw.getByRole("tabpanel").evaluate((element) => {
    element.scrollTop = element.scrollHeight;
  });
  await expect(
    raw.getByRole("button", { name: "Copy raw body", exact: true }),
  ).toBeVisible();
  const downloadEvent = page.waitForEvent("download");
  await raw.getByRole("button", { name: "Download", exact: true }).click();
  const downloaded = await downloadEvent;
  const bytes = await readFile(await downloaded.path(), "utf8");
  const request = page.waitForResponse(
    (response) =>
      response.url().includes("&part=response") && response.status() === 200,
  );
  await raw.getByRole("tab", { name: "Request", exact: true }).click();
  await expect(raw.getByRole("tabpanel")).toContainText('"tools"');
  await raw.getByRole("tab", { name: "Response", exact: true }).click();
  expect(bytes).toBe((await (await request).json()).text);
  await raw
    .getByRole("button", { name: "Close http request/response", exact: true })
    .click();

  await page.setViewportSize({ width: 390, height: 844 });
  await measure("mobile-compact");
  expect(metrics["mobile-compact"].transcript.height).toBeGreaterThanOrEqual(
    546,
  );
  await model.click();
  await expect(picker).toBeVisible();
  const mobilePanel = await picker.boundingBox();
  expect(mobilePanel.x).toBeGreaterThanOrEqual(0);
  expect(mobilePanel.x + mobilePanel.width).toBeLessThanOrEqual(390);
  await page.keyboard.press("Escape");
  await settingsButton.click();
  await settings.getByLabel("Display size", { exact: true }).fill("200");
  await settings.getByLabel("Text size", { exact: true }).fill("300");
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-settings-scaled.png`,
  });
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  await settings.getByRole("button", { name: "Close settings" }).click();
  await measure("mobile-scaled");
  expect(
    await prompt.evaluate((element) => {
      const style = getComputedStyle(element);
      return (
        element.clientHeight >=
        parseFloat(style.lineHeight) +
          parseFloat(style.paddingTop) +
          parseFloat(style.paddingBottom)
      );
    }),
  ).toBe(true);
  await settingsButton.click();
  await settings
    .getByRole("button", { name: "Reset sizes", exact: true })
    .click();
  await settings.getByRole("button", { name: "Close settings" }).click();
  await page.setViewportSize({ width: 844, height: 390 });
  await measure("landscape");
  expect(metrics.landscape.transcript.height).toBeGreaterThan(180);
  await model.click();
  await expect(picker).toBeVisible();
  await expect(
    picker.getByRole("combobox", { name: "Model", exact: true }),
  ).toBeVisible();
  const shortPanel = await picker.boundingBox();
  expect(shortPanel.y).toBeGreaterThanOrEqual(0);
  expect(shortPanel.y + shortPanel.height).toBeLessThanOrEqual(390);
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-landscape-picker.png`,
  });
  await page.keyboard.press("Escape");
  await page.setViewportSize({ width: 390, height: 450 });
  await measure("short-mobile");
  expect(metrics["short-mobile"].transcript.height).toBeGreaterThan(180);
  let releaseHistory;
  const historyGate = new Promise((resolve) => (releaseHistory = resolve));
  const sessionId = await page.evaluate(() =>
    new URLSearchParams(location.hash.slice(1)).get("session"),
  );
  await page.route(`**/api/sessions/${sessionId}`, async (route) => {
    await historyGate;
    await route.fulfill({
      status: 503,
      contentType: "application/json",
      body: JSON.stringify({ v: 1, error: "History temporarily unavailable" }),
    });
  });
  // WebKit does not intercept requests from a service-worker-controlled page.
  await page.evaluate(async () => {
    for (const registration of await navigator.serviceWorker.getRegistrations())
      await registration.unregister();
  });
  await page.reload();
  await expect(page.locator(".transcript .skeleton")).toBeVisible();
  await expect(
    page.getByRole("heading", { name: "What are we working on?" }),
  ).toHaveCount(0);
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-history-loading.png`,
  });
  releaseHistory();
  await expect(page.locator(".transcript").getByRole("alert")).toHaveText(
    "History temporarily unavailable",
  );
  await page.unroute(`**/api/sessions/${sessionId}`);
  await page
    .locator(".transcript")
    .getByRole("button", { name: "Retry", exact: true })
    .click();
  await expect(page.locator(".message")).toHaveCount(2);
  await writeFile(
    `test-results/${testInfo.project.name}-layout.json`,
    JSON.stringify(metrics, null, 2),
  );
  expect(errors).toEqual([]);
});

test("touch controls remain reachable at phone width", async ({
  browser,
}, testInfo) => {
  const state = JSON.parse(
    await readFile("test-results/device-state.json", "utf8"),
  );
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    storageState: state,
  });
  const page = await context.newPage();
  try {
    const catalogue = await (
      await context.request.get("http://127.0.0.1:8765/api/sessions")
    ).json();
    const session = catalogue.sessions.find(
      (item) => item.generation && !item.turn_active,
    );
    await page.goto(`http://127.0.0.1:8765/#session=${session.id}`);
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    expect(
      await page.evaluate(() => matchMedia("(pointer: coarse)").matches),
    ).toBe(true);
    for (const name of ["Model and effort", "Permissions", "Send"]) {
      const rect = await page
        .getByRole("button", { name, exact: true })
        .boundingBox();
      expect(Math.min(rect.width, rect.height)).toBeGreaterThanOrEqual(44);
      expect(rect.y + rect.height).toBeLessThanOrEqual(844);
    }
    await page
      .getByRole("button", { name: "Model and effort", exact: true })
      .tap();
    const picker = page.getByRole("dialog", {
      name: "Model and effort",
      exact: true,
    });
    await expect(
      picker.getByRole("combobox", { name: "Model", exact: true }),
    ).toBeVisible();
    const box = await picker.boundingBox();
    expect(box.x + box.width).toBeLessThanOrEqual(390);
    await page.screenshot({
      path: `test-results/${testInfo.project.name}-touch-picker.png`,
    });
    await picker.getByRole("button", { name: "Cancel", exact: true }).tap();
    await page.getByRole("button", { name: "Settings", exact: true }).tap();
    await page.getByLabel("Appearance").selectOption("dark");
    await page
      .getByRole("button", { name: "Close settings", exact: true })
      .tap();
    await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
    await page.screenshot({
      path: `test-results/${testInfo.project.name}-touch-dark.png`,
    });
  } finally {
    await context.close();
  }
});

test("polished skeletons, whole-row hover and folded tool output", async ({
  page,
  context,
}, testInfo) => {
  await context.addCookies(
    JSON.parse(await readFile("test-results/device-state.json", "utf8"))
      .cookies,
  );
  await page.route("**/sw.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "" }),
  );
  const catalogue = await (await context.request.get("/api/sessions")).json();
  const session = catalogue.sessions.find(
    (item) => item.presence === "web" && !item.turn_active,
  );
  const snapshot = await (
    await context.request.get(`/api/sessions/${session.id}`)
  ).json();
  snapshot.state.view.blocks = [
    {
      id: "fixture-tool",
      kind: "tool_result",
      name: "web_search",
      call_id: "fixture",
      detail_id: "t-fixture",
      arguments: { query: "a test query" },
      text: "short preview",
      status: "success",
      truncated: true,
      time: new Date().toISOString(),
    },
  ];
  snapshot.live = [];
  snapshot.state.context_tokens = 4600;
  snapshot.state.context_window = 1300000;
  await page.route(`**/api/sessions/${session.id}`, (route) =>
    route.fulfill({ json: snapshot }),
  );
  let releaseModel;
  const modelGate = new Promise((resolve) => (releaseModel = resolve));
  await page.route("**/assets/model-picker-*.js", async (route) => {
    await modelGate;
    await route.continue();
  });
  let requests = 0,
    releaseOutput;
  const outputGate = new Promise((resolve) => (releaseOutput = resolve));
  const full = JSON.stringify({
    request: { query: "a test query" },
    response: "retained output ".repeat(1500) + "END OF FULL RESULT",
  });
  await page.route(
    `**/api/sessions/${session.id}?detail=t-fixture*`,
    async (route) => {
      requests++;
      if (requests === 1) {
        await outputGate;
        return route.fulfill({
          status: 503,
          json: { error: "Tool output temporarily unavailable" },
        });
      }
      const offset = Number(
        new URL(route.request().url()).searchParams.get("offset"),
      );
      const next = Math.min(full.length, offset + 16384);
      await route.fulfill({
        json: {
          text: full.slice(offset, next),
          next,
          more: next < full.length,
        },
      });
    },
  );
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto(`/#session=${session.id}`);
  const tool = page.locator(".message.tool");
  await expect(tool).toBeVisible();
  await expect(
    page.getByRole("button", { name: "Show full tool output", exact: true }),
  ).toHaveCount(0);
  await expect(tool.locator(".tool-body")).toHaveCount(0);
  expect(requests).toBe(0);
  await expect(
    page.getByRole("button", { name: "Raw context", exact: true }),
  ).toHaveText("ctx 4.6k/1.3M · 99% left");
  const row = page
    .locator(".session-row")
    .filter({ has: page.locator(".session.selected") });
  await expect(row.locator("time")).toBeVisible();
  await expect(
    row.getByRole("img", { name: "Web session active" }),
  ).toBeVisible();
  const menu = row.getByRole("button", {
    name: "Conversation menu",
    exact: true,
  });
  await menu.hover();
  const hoverColor = await row.evaluate(
    (el) => getComputedStyle(el).backgroundColor,
  );
  expect(hoverColor).not.toBe("rgba(0, 0, 0, 0)");
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-sidebar-hover.png`,
  });
  await tool.locator(".tool-toggle").click();
  await expect(tool.getByRole("status")).toContainText(
    "Loading full tool output",
  );
  releaseOutput();
  await expect(tool.getByRole("alert")).toHaveText(
    "Tool output temporarily unavailable",
  );
  await tool.getByRole("button", { name: "Retry", exact: true }).click();
  await expect(tool.locator(".tool-body")).toContainText("END OF FULL RESULT");
  expect(requests).toBe(3);
  await tool.locator(".tool-toggle").click();
  await expect(tool.locator(".tool-body")).toHaveCount(0);
  await tool.locator(".tool-toggle").click();
  await expect(tool.locator(".tool-body")).toContainText("END OF FULL RESULT");
  expect(requests).toBe(3);
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-tool-expanded.png`,
  });
  await page
    .getByRole("button", { name: "Model and effort", exact: true })
    .click();
  const picker = page.getByRole("dialog", {
    name: "Model and effort",
    exact: true,
  });
  await expect(picker.getByRole("status")).toHaveAttribute("aria-busy", "true");
  await expect(picker.locator(".field-row > .field")).toHaveCount(2);
  await expect(
    picker.getByRole("button", { name: "Apply", exact: true }),
  ).toBeDisabled();
  const loadingBox = await picker.boundingBox();
  await page.screenshot({
    path: `test-results/${testInfo.project.name}-model-loading.png`,
  });
  releaseModel();
  await expect(
    picker.getByRole("combobox", { name: "Model", exact: true }),
  ).toBeVisible();
  const loadedBox = await picker.boundingBox();
  expect(Math.abs(loadedBox.height - loadingBox.height)).toBeLessThan(3);
  await page.keyboard.press("Escape");
});

test("late snapshots and retired streams cannot replace current session state", async ({
  page,
}) => {
  const epoch = "deterministic-host";
  const sessions = ["a", "b"].map((id) => ({
    id: id.repeat(16),
    generation: `generation-${id}`,
    cwd: "/mock/project",
    title: `Conversation ${id}`,
    status: "idle",
    presence: "web",
    incoming: 0,
  }));
  const snapshot = (metadata, context = 1000, cursor = 10) => ({
    v: 1,
    epoch,
    cursor,
    metadata,
    pending: null,
    state: {
      route: "mock/model-b",
      title: metadata.title,
      context_tokens: context,
      context_window: 1300000,
      view: {
        blocks: [
          {
            id: "m-1",
            kind: "assistant",
            text: metadata.title,
            truncated: true,
          },
        ],
      },
    },
  });
  await page.addInitScript(() => {
    globalThis.testStreams = [];
    globalThis.EventSource = class extends EventTarget {
      constructor() {
        super();
        globalThis.testStreams.push(this);
        queueMicrotask(() => this.onopen?.(new Event("open")));
      }
      close() {}
    };
  });
  await page.route("**/sw.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "" }),
  );
  let release;
  const held = new Promise((resolve) => (release = resolve));
  let requested;
  const reached = new Promise((resolve) => (requested = resolve));
  let holdB = true;
  await page.route("**/api/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path === "/api/sessions")
      return route.fulfill({
        json: {
          v: 1,
          epoch,
          cursor: 10,
          sessions,
          capabilities: {},
          devices: [],
        },
      });
    const session = sessions.find(
      (item) => path === `/api/sessions/${item.id}`,
    );
    if (!session) throw new Error(`Unexpected request: ${path}`);
    if (new URL(route.request().url()).searchParams.has("detail"))
      return route.fulfill({
        json: { text: `Expanded ${session.title}`, more: false, next: 0 },
      });
    if (session === sessions[1] && holdB) {
      requested();
      await held;
    }
    return route.fulfill({ json: snapshot(session) });
  });
  await page.goto(`/#session=${sessions[0].id}`);
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await page
    .getByRole("button", { name: "Show full message", exact: true })
    .click();
  await expect(page.locator(".message")).toContainText(
    "Expanded Conversation a",
  );
  const send = (index, metadata, context, sequence) =>
    page.evaluate(
      ({ index, event }) =>
        globalThis.testStreams[index].dispatchEvent(
          new MessageEvent("update", { data: JSON.stringify(event) }),
        ),
      {
        index,
        event: {
          v: 1,
          epoch,
          sequence,
          session_id: metadata.id,
          generation: metadata.generation,
          kind: "state",
          busy: false,
          state: snapshot(metadata, context).state,
        },
      },
    );
  await send(0, sessions[1], 5000, 11);
  await page.locator(".session").filter({ hasText: "Conversation b" }).click();
  await reached;
  // Stable message IDs are local to a conversation. Expanded text and tool
  // state from another conversation must never be reused for the same ID.
  await expect(page.locator(".message")).toContainText("Conversation b");
  await expect(page.locator(".message")).not.toContainText(
    "Expanded Conversation a",
  );
  await send(0, sessions[1], 9000, 12);
  const context = page.getByRole("button", {
    name: "Raw context",
    exact: true,
  });
  await expect(context).toContainText("ctx 9k/");
  holdB = false;
  const staleResponse = page.waitForResponse(
    (response) =>
      new URL(response.url()).pathname === `/api/sessions/${sessions[1].id}`,
  );
  release();
  await staleResponse;
  await page.evaluate(
    () =>
      new Promise((resolve) =>
        requestAnimationFrame(() => requestAnimationFrame(resolve)),
      ),
  );
  await expect(context).toContainText("ctx 9k/");
  // A repeated/out-of-order event must not roll back the same generation.
  await send(0, sessions[1], 3000, 11);
  await expect(context).toContainText("ctx 9k/");
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect
    .poll(() => page.evaluate(() => globalThis.testStreams.length))
    .toBe(2);
  await send(1, sessions[1], 12000, 13);
  await expect(context).toContainText("ctx 12k/");
  await send(0, sessions[1], 800000, 99);
  await page.evaluate(() =>
    globalThis.testStreams[0].onerror(new Event("error")),
  );
  await expect(context).toContainText("ctx 12k/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await expect(page.locator(".error-banner")).toHaveCount(0);
});

test("keyboard viewport preserves focus and contains chat, dialogs and editors", async ({
  browser,
}, testInfo) => {
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
    storageState: "test-results/device-state.json",
  });
  const page = await context.newPage(),
    errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  await page.route("**/sw.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "" }),
  );
  // Desktop engines cannot open a phone keyboard. Model an independently
  // resized/panned visual viewport; a window resize alone misses this bug.
  const viewport = async (height, top = 0, scale = 1) => {
    await page.evaluate(
      ({ height, top, scale }) => {
        for (const [key, value] of Object.entries({
          height,
          offsetTop: top,
          scale,
        }))
          Object.defineProperty(visualViewport, key, {
            configurable: true,
            value,
          });
        visualViewport.dispatchEvent(new Event("resize"));
        visualViewport.dispatchEvent(new Event("scroll"));
      },
      { height, top, scale },
    );
    if (scale === 1)
      await expect
        .poll(async () =>
          Math.round((await page.locator("#app").boundingBox()).height),
        )
        .toBe(height);
  };
  const contained = async (locator, height, top = 0) => {
    await expect
      .poll(async () => {
        const box = await locator.boundingBox();
        return (
          !!box &&
          box.y >= top - 1 &&
          box.y + box.height <= top + height + 1 &&
          box.x >= -1 &&
          box.x + box.width <= 391
        );
      })
      .toBe(true);
  };
  const input = async (locator, text) => {
    await locator.fill(text);
    await viewport(390, 70);
    await expect(locator).toBeFocused();
    expect(
      await locator.evaluate((e) => parseFloat(getComputedStyle(e).fontSize)),
    ).toBeGreaterThanOrEqual(16);
    await contained(locator, 390, 70);
  };
  try {
    const catalogue = await (
      await context.request.get("http://127.0.0.1:8765/api/sessions")
    ).json();
    const session = catalogue.sessions.find(
      (s) => s.generation && !s.turn_active && !s.pending,
    );
    await page.goto(`http://127.0.0.1:8765/#session=${session.id}`);
    const prompt = page.getByLabel("Message or guidance");
    await expect(prompt).toBeVisible();
    expect((await page.locator(".composer").boundingBox()).height).toBeLessThan(
      150,
    );
    const draft = "Keep my draft and focus as the keyboard moves";
    await input(prompt, draft);
    for (const [height, top] of [
      [330, 110],
      [460, 30],
      [844, 0],
    ]) {
      await viewport(height, top);
      await contained(page.locator(".composer"), height, top);
      await expect(prompt).toHaveValue(draft);
      await expect(prompt).toBeFocused();
      expect(await page.evaluate(() => scrollY)).toBe(0);
    }
    await viewport(422, 100, 2);
    expect(Math.round((await page.locator("#app").boundingBox()).height)).toBe(
      844,
    );
    await viewport(430, 60);
    await page
      .getByRole("button", { name: "Model and effort", exact: true })
      .tap();
    const picker = page.getByRole("dialog", {
      name: "Model and effort",
      exact: true,
    });
    await contained(picker, 430, 60);
    // Safari also sends pans without resize events.
    await page.evaluate(() => {
      Object.defineProperty(visualViewport, "offsetTop", {
        configurable: true,
        value: 90,
      });
      visualViewport.dispatchEvent(new Event("scroll"));
    });
    await expect
      .poll(async () =>
        Math.round((await page.locator("#app").boundingBox()).y),
      )
      .toBe(90);
    await contained(picker, 430, 90);
    await picker.getByRole("button", { name: "Cancel", exact: true }).tap();
    await viewport(844);
    await page.getByRole("button", { name: "Settings", exact: true }).tap();
    const settings = page.getByRole("dialog", {
      name: "Settings",
      exact: true,
    });
    await settings.getByLabel("Display size", { exact: true }).fill("50");
    await settings.getByLabel("Text size", { exact: true }).fill("50");
    await settings
      .getByRole("button", { name: "Advanced configuration", exact: true })
      .tap();
    await input(settings.getByLabel("Find a setting"), "web");
    await contained(settings, 390, 70);
    await page.screenshot({
      path: `test-results/${testInfo.project.name}-keyboard-settings.png`,
    });
    await settings.getByRole("button", { name: "← Back", exact: true }).tap();
    await settings
      .getByRole("button", { name: "Reset sizes", exact: true })
      .tap();
    await settings
      .getByRole("button", { name: "Close settings", exact: true })
      .tap();
    await viewport(844);
    await page
      .getByRole("button", { name: "Open sessions", exact: true })
      .tap();
    const drawer = page.getByRole("dialog", { name: "Sessions", exact: true });
    await input(drawer.getByRole("searchbox"), "");
    await contained(drawer, 390, 70);
    await drawer.getByRole("button", { name: "Library", exact: true }).tap();
    await viewport(844);
    await page.getByRole("button", { name: "Add memory", exact: true }).tap();
    await input(page.getByLabel("Name", { exact: true }), "keyboard draft");
    await input(
      page.getByLabel("Document content", { exact: true }),
      "Editable above the keyboard.",
    );
    await page.screenshot({
      path: `test-results/${testInfo.project.name}-keyboard-memory.png`,
    });
    await viewport(844);
    await page
      .getByRole("button", { name: "Open sessions", exact: true })
      .tap();
    await drawer.getByRole("button", { name: /^Scheduled/ }).tap();
    await page.getByRole("button", { name: "New task", exact: true }).tap();
    await input(
      page.getByLabel("Instructions", { exact: true }),
      "Do not schedule this draft.",
    );
    await page.screenshot({
      path: `test-results/${testInfo.project.name}-keyboard-schedule.png`,
    });
    await viewport(844);
    await input(page.getByLabel("Timezone", { exact: true }), "UTC");
    await viewport(844);
    expect(
      await page.evaluate(
        () => document.documentElement.scrollWidth <= innerWidth,
      ),
    ).toBe(true);
    expect(errors).toEqual([]);
  } finally {
    await context.close();
  }
});
