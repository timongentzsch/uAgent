import {
  test,
  expect,
  online,
  withoutServiceWorker,
  newFolder,
  headerAction,
} from "./fixtures.js";
import { readFile, writeFile } from "node:fs/promises";

test("instructions are one stack, edited in place, in a stable dialog", async ({
  page,
  host: fixture,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await newFolder(page, fixture.project);
  await page
    .getByRole("button", { name: "Start conversation", exact: true })
    .click();
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  const composer = page.getByLabel("Message or guidance");
  await composer.fill("/instructions");
  await composer.press("Enter");
  const dialog = page.getByRole("dialog", {
    name: "Instructions",
    exact: true,
  });
  // Top to bottom in the order a session reads it.
  await expect(dialog.getByLabel("Yours · every conversation")).toBeEnabled();
  const headings = await dialog
    .locator(
      ".instructions summary, .instruction-card strong, .instructions h3",
    )
    .allTextContents();
  expect(headings).toEqual([
    "Built-in base",
    "Yours · every conversation",
    "Project · every conversation",
    "Panel",
    "Built-in panel base",
    "Yours · panel",
    "Project · panel",
    "Chat members",
    "Built-in member base",
  ]);
  const yours = dialog.getByLabel("Yours · every conversation");
  await yours.fill("Prefer small diffs.\nKeep this line break.");
  await dialog.getByRole("button", { name: "Save", exact: true }).click();
  await expect(dialog.getByRole("button", { name: "Save" })).toHaveCount(0);
  await dialog.getByRole("button", { name: "Close instructions" }).click();
  await composer.fill("/instructions");
  await composer.press("Enter");
  await expect(dialog.getByLabel("Yours · every conversation")).toHaveValue(
    "Prefer small diffs.\nKeep this line break.",
  );
  // Classic scrollbar gutters, even on hosts with overlay scrollbars.
  await page.addStyleTag({
    content:
      "dialog > header::-webkit-scrollbar, .dialog-body::-webkit-scrollbar { width: 1rem; }",
  });
  for (const viewport of [
    { width: 390, height: 600 },
    { width: 844, height: 390 },
    { width: 1280, height: 800 },
  ]) {
    await page.setViewportSize(viewport);
    const safeTop = viewport.width < 600 ? 47 : 0;
    const safeBottom = viewport.width < 600 ? 34 : 21;
    await page.evaluate(
      ({ top, bottom }) => {
        const style = document.documentElement.style;
        style.setProperty("--safe-top", `${top}px`);
        style.setProperty("--safe-bottom", `${bottom}px`);
      },
      { top: safeTop, bottom: safeBottom },
    );
    await expect(page.locator("html")).toHaveCSS(
      "--viewport-width",
      `${viewport.width}px`,
    );
    const body = dialog.locator(".dialog-body");
    // A sheet runs from the safe top to the bottom edge, its content above
    // the home indicator.
    const bounds = await dialog.boundingBox();
    expect(bounds.y).toBeGreaterThanOrEqual(safeTop);
    expect(bounds.y + bounds.height).toBeLessThanOrEqual(viewport.height);
    await body.evaluate((element) => element.scrollTo(0, 0));
    const header = await dialog.locator("header").first().boundingBox();
    // Scroll the body itself: the wheel over an editor scrolls the editor.
    await body.evaluate((element) => element.scrollTo(0, element.scrollHeight));
    await expect
      .poll(() =>
        body.evaluate(
          (element) =>
            element.scrollHeight - element.clientHeight - element.scrollTop,
        ),
      )
      .toBeLessThanOrEqual(1);
    expect(await dialog.locator("header").first().boundingBox()).toEqual(
      header,
    );
    expect(
      await dialog.evaluate(
        (element) => element.scrollWidth <= element.clientWidth + 1,
      ),
    ).toBe(true);
  }
  await page.setViewportSize({ width: 1280, height: 800 });
  // The request view links back to the instructions.
  await dialog.getByRole("button", { name: "Close instructions" }).click();
  await page.getByRole("button", { name: "Raw context", exact: true }).click();
  await page
    .getByRole("dialog", { name: "Raw context" })
    .getByRole("button", { name: "Instructions", exact: true })
    .click();
  await expect(dialog.getByLabel("Yours · every conversation")).toBeVisible();
  await dialog.getByRole("button", { name: "Close instructions" }).click();
});

test("compact surfaces stay anchored, accessible and usable while loading", async ({
  page,
  host: fixture,
}, testInfo) => {
  await withoutServiceWorker(page);
  await page.setViewportSize({ width: 1440, height: 1000 });
  // Held from the start: the app loads this code by itself two seconds in,
  // and the loading state below is only there to see while it is held.
  let releaseSettings;
  const settingsGate = new Promise((resolve) => (releaseSettings = resolve));
  // The dialog's chunk; settings-nav-* and settings-scopes-* belong to the shell and must load.
  await page.route(
    /\/assets\/settings-(?!nav-|scopes-)[^/]+\.js$/,
    async (route) => {
      await settingsGate;
      await route.continue();
    },
  );
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();

  await newFolder(page, fixture.project);
  await page
    .getByRole("button", { name: "Start conversation", exact: true })
    .click();
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  const prompt = page.getByLabel("Message or guidance");
  const model = page.getByRole("button", {
    name: "Model and effort",
    exact: true,
  });
  await expect(model).toBeVisible();
  const metricButtons = page.locator(".metrics > button");
  await expect(metricButtons).toHaveCount(2);
  const metricBoxes = await metricButtons.evaluateAll((buttons) =>
    buttons.map((button) => {
      const box = button.getBoundingClientRect();
      const style = getComputedStyle(button);
      return {
        height: box.height,
        paddingLeft: style.paddingLeft,
        paddingRight: style.paddingRight,
      };
    }),
  );
  expect(metricBoxes[0]).toEqual(metricBoxes[1]);
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
      // Rows off screen may still report the pre-resize layout for a frame,
      // so wait for the two to settle like the composer check above.
      await expect
        .poll(async () => {
          const user = await page
            .locator(".message.user")
            .first()
            .boundingBox();
          const response = await page
            .locator(".message.response")
            .first()
            .boundingBox();
          return Math.max(
            Math.abs(user.x - response.x),
            Math.abs(user.width - response.width),
          );
        })
        .toBeLessThan(2);
      await expect(page.locator(".message.user").first()).toHaveCSS(
        "text-align",
        "left",
      );
      await expect(
        page.getByRole("button", { name: /^Reply to:/ }),
      ).toHaveCount(0);
    }
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
  // It is a sheet at the window's right edge, not a panel beside its button.
  await expect
    .poll(async () => {
      const box = await picker.boundingBox();
      const width = page.viewportSize()?.width ?? 0;
      return box ? Math.abs(box.x + box.width - width) : Infinity;
    })
    .toBeLessThan(2);
  await page.keyboard.press("Escape");
  await expect(picker).toHaveCount(0);
  await expect(model).toBeFocused();
  await model.click();
  await expect(picker).toBeVisible();
  // A tap on the scrim, over the composer, closes the sheet.
  const promptBox = await prompt.boundingBox();
  await page.mouse.click(promptBox.x + 10, promptBox.y + 10);
  await expect(picker).toHaveCount(0);
  await expect(prompt).toHaveValue("Keep this draft through popups");

  const settingsButton = page.getByRole("button", {
    name: "Settings",
    exact: true,
  });
  await settingsButton.focus();
  await page.keyboard.press("Enter");
  const settings = page.getByRole("dialog", { name: "Settings", exact: true });
  await expect(settings).toBeVisible();
  await expect(
    settings.getByRole("status", { name: "Loading settings…" }),
  ).toHaveAttribute("aria-busy", "true");
  await expect(
    settings.getByRole("heading", { name: "Settings", exact: true }),
  ).toBeFocused();
  // The skeleton can report a mid-growth box on the opening frames
  // (identical DOM measures 394 then settles at 572), so snapshot it
  // once two consecutive frames agree instead of sampling one frame.
  let skeletonLast = -1;
  await expect
    .poll(async () => {
      const height = (await settings.boundingBox())?.height ?? -1;
      const settled =
        skeletonLast >= 0 && Math.abs(height - skeletonLast) < 0.5;
      skeletonLast = height;
      return settled ? height : -1;
    })
    .toBeGreaterThan(0);
  const settingsLoadingBox = await settings.boundingBox();
  releaseSettings();
  // The first section loaded: what applies to all conversations.
  await expect(settings.getByText("Detail shown")).toBeVisible();
  // The loaded form fills in over staged renders (chunk, then config
  // data), so pin parity once the settled height matches instead of
  // sampling a mid-render frame. The invariant is unchanged.
  await expect
    .poll(async () =>
      Math.abs(
        (await settings.boundingBox()).height - settingsLoadingBox.height,
      ),
    )
    .toBeLessThan(6);
  await settings
    .locator(".settings-nav")
    .getByRole("button", { name: "This browser", exact: true })
    .click();
  await expect(settings.getByLabel("Timestamps")).toBeVisible();
  const appearance = await settings.getByLabel("Appearance").boundingBox();
  const displayField = await settings
    .getByLabel("Zoom", { exact: true })
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
    .fill("model-b");
  await page.getByRole("option").filter({ hasText: "mock/model-b" }).click();
  await picker.getByRole("button", { name: "Apply", exact: true }).click();
  await expect(picker).toHaveCount(0);
  await expect(model).toContainText("mock/model-b");
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
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  await expect(
    page.getByRole("button", { name: "Raw context", exact: true }),
  ).toContainText("/1.3M · 99% left");
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
  await headerAction(page, "Settings");
  await settings
    .locator(".settings-nav")
    .getByRole("button", { name: "This browser", exact: true })
    .click();
  await settings.getByLabel("Zoom", { exact: true }).fill("200");
  await expect
    .poll(() =>
      page.evaluate(() =>
        parseFloat(getComputedStyle(document.documentElement).fontSize),
      ),
    )
    .toBeGreaterThan(27);
  const dialogGutter = await settings.evaluate((element) => {
    const bounds = element.getBoundingClientRect();
    const app = document.getElementById("app");
    const appBounds = app.getBoundingClientRect();
    const appStyle = getComputedStyle(app);
    return {
      left: bounds.left - appBounds.left - parseFloat(appStyle.paddingLeft),
      right: appBounds.right - parseFloat(appStyle.paddingRight) - bounds.right,
    };
  });
  // Edge to edge on a phone, at any zoom: a sheet, not an inset card.
  expect(Math.abs(dialogGutter.left)).toBeLessThan(1);
  expect(Math.abs(dialogGutter.right)).toBeLessThan(1);
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
      // +1px: integer scrollHeight autosize can undershoot fractional
      // line/padding sums by a sub-pixel at scaled zooms.
      return (
        element.clientHeight + 1 >=
        parseFloat(style.lineHeight) +
          parseFloat(style.paddingTop) +
          parseFloat(style.paddingBottom)
      );
    }),
  ).toBe(true);
  await headerAction(page, "Settings");
  await settings
    .locator(".settings-nav")
    .getByRole("button", { name: "This browser", exact: true })
    .click();
  await settings.getByRole("button", { name: /^Zoom .*reset$/ }).click();
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
      body: JSON.stringify({ v: 2, error: "History temporarily unavailable" }),
    });
  });
  // WebKit does not intercept requests from a service-worker-controlled page.
  await page.evaluate(async () => {
    for (const registration of await navigator.serviceWorker.getRegistrations())
      await registration.unregister();
  });
  await page.reload();
  // While history is held the transcript shows the announced loading
  // mirror (many decorative shimmer bars share .skeleton, so pin the
  // singular status region instead of the strict-violating class).
  await expect(page.locator(".transcript [role='status']")).toBeVisible();
  await expect(
    page.getByRole("heading", { name: "What are we working on?" }),
  ).toHaveCount(0);
  releaseHistory();
  await expect(page.locator(".transcript").getByRole("alert")).toHaveText(
    "History temporarily unavailable",
  );
  await page.unroute(`**/api/sessions/${sessionId}`);
  // A reconnect can restore history before the manual retry is dispatched.
  await page
    .locator(".transcript")
    .getByRole("button", { name: "Retry", exact: true })
    .evaluateAll((buttons) => buttons[0]?.click());
  await expect(page.locator(".message")).toHaveCount(2);
  await writeFile(
    testInfo.outputPath(`layout.json`),
    JSON.stringify(metrics, null, 2),
  );
});

test("code blocks, thinking and HTTP dialogs preserve content and loading geometry", async ({
  page,
  session,
  command,
}) => {
  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
  await withoutServiceWorker(page);
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("HTTP body proof");
  await prompt.press("Enter");
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  const reply = page.locator(".message.response").last();
  // Tailnet HTTP has no Clipboard API; a user click must still copy.
  await page.evaluate(() =>
    Object.defineProperty(navigator, "clipboard", { value: undefined }),
  );
  const code = reply.locator(".code-block");
  const copy = code.getByRole("button", { name: "Copy code", exact: true });
  await expect(copy).toHaveCount(1);
  await copy.focus();
  await expect(code.locator(".code-copy")).toHaveCSS("opacity", "1");
  await copy.press("Enter");
  await expect(
    reply.getByRole("button", { name: "Copied!", exact: true }),
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
  await page.emulateMedia({ colorScheme: "light" });

  // An answer's menu is on its turn's last line, under it.
  await page
    .locator(".turn-footer")
    .last()
    .getByRole("button", { name: /^Actions for / })
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
  let releaseRawBody;
  const bodyGate = new Promise((resolve) => (releaseRawBody = resolve));
  await page.route("**/api/sessions/*?http=*", async (route) => {
    await bodyGate;
    await route.continue();
  });
  await rawAction.click();
  const raw = page.getByRole("dialog", {
    name: "HTTP request/response",
    exact: true,
  });
  await expect(raw.getByRole("status")).toHaveAttribute("aria-busy", "true");
  // The eager dialog shell reserves geometry while unknown code loads.
  // Once loaded, the real controls stay present while its body data loads.
  await expect(raw.getByRole("status")).toBeVisible();
  const rawLoadingBox = await raw.boundingBox();
  releaseRaw();
  await expect(
    raw
      .locator(".raw-body")
      .getByRole("status", { name: "Loading full body…" }),
  ).toBeVisible();
  await expect(raw.locator(".raw-content")).toHaveCount(1);
  await expect(raw.locator(".raw-body")).toHaveCount(1);
  releaseRawBody();
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

  await raw.getByRole("tab", { name: "Response", exact: true }).click();
  await expect(raw.getByRole("tabpanel")).toContainText("[DONE]");
  await expect(
    raw.getByRole("button", { name: "Events", exact: true }),
  ).toHaveAttribute("aria-pressed", "true");
  await expect(raw.locator(".raw-body > pre")).not.toContainText("data:");
  await raw.getByRole("button", { name: "Source", exact: true }).click();
  await expect(raw.locator(".raw-body > pre")).toContainText("data: [DONE]");
  await raw.getByRole("button", { name: "Events", exact: true }).click();
  await raw.getByRole("tabpanel").evaluate((element) => {
    element.scrollTop = element.scrollHeight;
  });
  await expect(raw.getByRole("button", { name: /Copy/ })).toHaveCount(0);
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
});

test("polished skeletons, whole-row hover and folded tool output", async ({
  page,
  context,
  session,
  host: fixture,
}) => {
  await withoutServiceWorker(page);
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
      view: {
        input: [
          { kind: "command", text: `psql -c "SELECT 'x'"` },
          { kind: "fields", rows: [["query", "a test query"]] },
        ],
        output: "text",
      },
      text: "short preview",
      reasoning: "Closed rich reasoning: $$x^2 + y^2$$",
      status: "success",
      truncated: true,
      time: new Date().toISOString(),
      activity: {
        label: `ssh remote "docker exec -i app python manage.py shell" <<'PY' 2>&1 | grep '^SMOKE' | ${"x".repeat(200)}`,
        category: "execute",
        status: "success",
        duration_ms: 2300,
      },
    },
  ];
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
  // Tool rows are headerless by design (attribution lives on response
  // rows); the disclosure summary is the row's top edge, and it carries
  // no mark (the inline-icon removal).
  await expect(tool.locator(":scope > header")).toHaveCount(0);
  await expect(tool.locator(".tool-disclosure > summary .mark")).toHaveCount(0);
  // Atomic read: the stream can shift rows between two boundingBox
  // calls (skeleton swaps), inverting a split comparison.
  const edges = await tool.evaluate((element) => {
    const summary = element.querySelector(".tool-disclosure > summary");
    const row = element.getBoundingClientRect();
    const head = summary?.getBoundingClientRect();
    return { toolY: row.y, summaryY: head?.y ?? row.y };
  });
  expect(edges.summaryY).toBeGreaterThanOrEqual(edges.toolY);
  // However long the label grows, it takes the ellipsis and the status
  // metadata stays on one line.
  const toggle = tool.locator(".tool-disclosure > summary");
  await expect(toggle.locator(".disclosure-label")).toHaveCSS(
    "text-overflow",
    "ellipsis",
  );
  await expect(toggle.locator("small")).toHaveCSS("white-space", "nowrap");
  expect(
    await toggle
      .locator("small")
      .evaluate((element) => element.scrollHeight <= element.clientHeight + 1),
  ).toBe(true);
  expect(
    await page.locator(".transcript").evaluate(async (element) => {
      const children = [...element.children];
      for (const child of children) child.style.display = "none";
      const spacer = document.createElement("div");
      spacer.style.height = "240px";
      spacer.style.flex = "none";
      element.append(spacer);
      element.style.flex = "none";
      element.style.height = "200px";
      element.scrollTop = element.scrollHeight;
      element.dispatchEvent(new Event("scroll"));
      await new Promise(requestAnimationFrame);
      element.scrollTop = 0;
      element.dispatchEvent(new Event("scroll"));
      element.style.height = "199px";
      await new Promise(requestAnimationFrame);
      element.style.height = "200px";
      await new Promise(requestAnimationFrame);
      await new Promise(requestAnimationFrame);
      const top = element.scrollTop;
      spacer.remove();
      element.style.flex = "";
      element.style.height = "";
      for (const child of children) child.style.display = "";
      return top;
    }),
  ).toBe(0);
  await expect(
    page.getByRole("button", { name: "Show full tool output", exact: true }),
  ).toHaveCount(0);
  await expect(tool.locator(".tool-body")).toHaveCount(0);
  await expect(tool.locator(".thinking .markdown")).toHaveCount(0);
  await expect(tool.locator(".katex")).toHaveCount(0);
  expect(requests).toBe(0);
  const row = page
    .locator(".session-row")
    .filter({ has: page.locator(".session[aria-current]") });
  // One line: the time and the menu show while the row is pointed at.
  await expect(row.locator("time")).toBeHidden();
  await row.hover();
  await expect(row.locator("time")).toBeVisible();
  const menu = row.getByRole("button", {
    name: "Conversation menu",
    exact: true,
  });
  await menu.hover();
  const hoverColor = await row.evaluate(
    (el) => getComputedStyle(el).backgroundColor,
  );
  expect(hoverColor).not.toBe("rgba(0, 0, 0, 0)");
  await toggle.click();
  await expect(tool.getByRole("status")).toContainText(
    "Loading full tool output",
  );
  await expect(tool.locator(".loading-label")).toBeVisible();
  releaseOutput();
  await expect(tool.getByRole("alert")).toHaveText(
    "Tool output temporarily unavailable",
  );
  await tool.getByRole("button", { name: "Retry", exact: true }).click();
  await expect(tool.locator(".tool-body")).toContainText("END OF FULL RESULT");
  expect(requests).toBe(3);
  // The native view reads as typed: the command verbatim, never escaped JSON.
  await expect(tool.locator(".tool-command")).toHaveText(
    `psql -c "SELECT 'x'"`,
  );
  // Short arguments read as facts in the info line, not as field rows.
  await expect(tool.locator(".tool-facts")).toContainText("a test query");
  await expect(tool.locator(".tool-fields")).toHaveCount(0);
  await expect(tool.locator(".thinking .markdown")).toHaveCount(0);
  await expect(tool.locator(".katex")).toHaveCount(0);
  await toggle.click();
  await expect(tool.locator(".tool-body")).toBeHidden();
  await toggle.click();
  await expect(tool.locator(".tool-body")).toContainText("END OF FULL RESULT");
  expect(requests).toBe(3);
  await page
    .getByRole("button", { name: "Model and effort", exact: true })
    .click();
  const picker = page.getByRole("dialog", {
    name: "Model and effort",
    exact: true,
  });
  await expect(picker.getByRole("status")).toHaveAttribute("aria-busy", "true");
  await expect(picker.locator(".field-row > .field")).toHaveCount(1);
  await expect(
    picker.getByRole("button", { name: "Apply", exact: true }),
  ).toBeDisabled();
  const loadingBox = await picker.boundingBox();
  await page.emulateMedia({ reducedMotion: "reduce" });
  for (const theme of ["light", "dark"]) {
    await page.evaluate((value) => {
      document.documentElement.dataset.theme = value;
    }, theme);
    await page.setViewportSize({ width: 390, height: 600 });
    await expect(picker.getByRole("status")).toHaveAccessibleName(
      "Loading models…",
    );
    await expect(picker.locator(".placeholder select")).toHaveCount(1);
  }
  await page.evaluate(() => {
    document.documentElement.dataset.theme = "light";
  });
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await page.setViewportSize({ width: 1440, height: 1000 });
  releaseModel();
  await expect(
    picker.getByRole("combobox", { name: "Model", exact: true }),
  ).toBeVisible();
  const loadedBox = await picker.boundingBox();
  expect(Math.abs(loadedBox.height - loadingBox.height)).toBeLessThan(3);
  await page.keyboard.press("Escape");
});
