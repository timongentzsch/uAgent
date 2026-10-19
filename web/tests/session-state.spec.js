import { test, expect, online, withoutServiceWorker } from "./fixtures.js";

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
    presence: "active",
    incoming: 0,
  }));
  const snapshot = (metadata, context = 1000, cursor = 10) => ({
    v: 2,
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
  await withoutServiceWorker(page);
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
          v: 2,
          epoch,
          cursor: 10,
          sessions,
          capabilities: {},
          devices: [],
        },
      });
    if (path === "/api/browser/status")
      return route.fulfill({ status: 404, json: { error: "Not found" } });
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
  await expect
    .poll(() => page.evaluate(() => globalThis.testStreams.length))
    .toBe(1);
  const ready = (index, cursor) =>
    page.evaluate(
      ({ index, epoch, cursor }) =>
        globalThis.testStreams[index].dispatchEvent(
          new MessageEvent("ready", {
            data: JSON.stringify({ epoch, cursor }),
          }),
        ),
      { index, epoch, cursor },
    );
  await ready(0, 10);
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
          v: 2,
          epoch,
          sequence,
          session_id: metadata.id,
          generation: metadata.generation,
          kind: "state",
          busy: false,
          metadata,
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
  await ready(1, 12);
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

test("tool rows and memory receipts survive reload and mobile rotation", async ({
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
  await command("permissions", {
    session_id: session.id,
    generation: session.generation,
    mode: "yolo",
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Exploration probe");
  await prompt.press("Enter");
  // The host grouped the two reads: one row under its label, as a terminal
  // shows them.
  await expect(page.locator(".transcript .tool-disclosure")).toHaveCount(1);
  await expect(page.locator(".transcript .group")).toHaveText(
    /Explored · 2 calls/,
  );
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  // The conversation menu sets the level, and what is shown restyles: every
  // call at full, the whole turn's work in one row at minimal.
  const detail = async (level) => {
    await page
      .locator(".conversation-head")
      .getByLabel("Conversation menu", { exact: true })
      .click();
    // One row opens the levels in place, with a way back.
    await page.getByRole("menuitem", { name: /^Detail/ }).click();
    await expect(page.getByText("For all conversations.")).toBeVisible();
    await page.getByRole("menuitemradio", { name: level, exact: true }).click();
  };
  await detail("Full");
  await expect(page.locator(".transcript .group")).toHaveCount(0);
  await expect(page.locator(".transcript .tool-disclosure")).toHaveCount(2);
  await detail("Minimal");
  await expect(page.locator(".transcript .group")).toHaveText(
    /Worked · 2 steps/,
  );
  await detail("Default");
  await expect(page.locator(".transcript .group")).toHaveText(
    /Explored · 2 calls/,
  );
  // Tables keep their layout and scroll instead of squeezing a column.
  const table = page.locator(".message .table-scroll > table");
  await expect(table).toHaveCount(1);
  await expect(table).toHaveCSS("display", "table");
  const cell = table.locator("td").first();
  await expect(cell).toBeVisible();
  const cellRatio = await cell.evaluate(
    (element) =>
      element.getBoundingClientRect().height /
      parseFloat(getComputedStyle(element).fontSize),
  );
  expect(cellRatio).toBeLessThan(4);
  const title = page.locator(".tool-disclosure .disclosure-label").first();
  await title.evaluate((element) => {
    element.textContent = "long-command-".repeat(200);
  });
  const row = await title.locator("..").boundingBox();
  expect(row.height).toBeLessThan(60);
  expect(
    await page.evaluate(() => document.documentElement.scrollWidth),
  ).toBeLessThanOrEqual(391);
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  await prompt.fill("Memory receipt probe: please save this test memory");
  await prompt.press("Enter");
  await expect(
    page
      .locator(".tool-disclosure")
      .filter({ hasText: "Saved memory project/browser-proof" }),
  ).toBeVisible();
  // Its detail reads top-down: key, one caption, then the memory itself
  // directly below (not floated to the middle of the sheet).
  await page.getByRole("button", { name: "Open memory" }).first().click();
  const memory = page.getByRole("dialog", { name: "Memory" });
  await expect(memory).toBeVisible();
  const section = memory.getByRole("region", { name: "Memory" });
  await expect(section.locator(".detail-label")).toHaveText(
    "project/browser-proof",
  );
  await expect(memory.getByText(/Current memory/)).toHaveCount(0);
  const caption = await section.locator(".detail-meta").boundingBox();
  const body = await section.locator(".markdown").boundingBox();
  expect(body.y - (caption.y + caption.height)).toBeLessThan(24);
  await page.keyboard.press("Escape");
  await expect(memory).toHaveCount(0);
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  await page.locator(".transcript").evaluate((element) => {
    element.scrollTop = 0;
  });
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeVisible();
  await page.reload();
  await expect(page.locator(".transcript > * .tool-disclosure")).toHaveCount(2);
  // Retained history replays through the live pipeline: the same group and
  // the receipt, none stuck on Running.
  await expect(
    page.locator(".transcript .tool-disclosure", { hasText: "Running" }),
  ).toHaveCount(0);
  await expect(
    page
      .locator(".tool-disclosure")
      .filter({ hasText: "Saved memory project/browser-proof" }),
  ).toBeVisible();
  await expect
    .poll(() =>
      page.locator(".transcript").evaluate((element) => element.scrollTop),
    )
    .toBeLessThan(2);
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeVisible();
  await prompt.fill("Keep this draft across rotation");
  const fontSize = await prompt.evaluate(
    (element) => getComputedStyle(element).fontSize,
  );
  await page.setViewportSize({ width: 844, height: 390 });
  await page.evaluate(() => {
    document.documentElement.style.setProperty("--safe-left", "47px");
    document.documentElement.style.setProperty("--safe-right", "47px");
  });
  await expect(prompt).toHaveValue("Keep this draft across rotation");
  await expect(prompt).toHaveCSS("font-size", fontSize);
  const box = await prompt.boundingBox();
  expect(box.x).toBeGreaterThanOrEqual(47);
  expect(box.x + box.width).toBeLessThanOrEqual(844 - 47);
  await page.setViewportSize({ width: 390, height: 844 });
  await page.evaluate(() => {
    document.documentElement.style.removeProperty("--safe-left");
    document.documentElement.style.removeProperty("--safe-right");
  });
  await expect(prompt).toHaveValue("Keep this draft across rotation");
  await expect(prompt).toHaveCSS("font-size", fontSize);
});

test("stream completion preserves disclosure, markdown nodes, selection and copy state", async ({
  page,
  host: fixture,
  session,
  command,
  request,
}, testInfo) => {
  test.setTimeout(45_000);
  await page.addInitScript(() => {
    Object.defineProperty(navigator, "clipboard", {
      configurable: true,
      value: { writeText: async () => {} },
    });
  });
  await page.goto(fixture.origin);
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
  await page.goto(`/#session=${session.id}`);
  await expect(page).toHaveURL(new RegExp(session.id));
  await expect(page.locator(".conversation-head h1")).toBeVisible();
  const prompt = page.getByLabel("Message or guidance");
  await expect(prompt).toBeVisible();
  await prompt.fill("Long continuity probe");
  await prompt.press("Enter");
  await expect(page.locator(".composer .status-led.running")).toBeVisible();

  const thinking = page.locator(".thinking");
  await expect(thinking).toBeVisible();
  // A never-opened disclosure has no Markdown tree or optional renderer work.
  await expect(thinking.locator(".markdown")).toHaveCount(0);
  await thinking.locator("summary").click();
  await expect(thinking.locator(".markdown")).toBeVisible();

  const opening = page
    .locator(".message.response > .markdown p")
    .filter({ hasText: "Stable opening paragraph" });
  await expect(opening).toBeVisible();
  const completedBlock = opening.locator("..");
  await completedBlock.evaluate((node) => {
    window.__completedMarkdownBlock = node;
  });

  const copy = page.getByRole("button", { name: "Copy code" }).first();
  await expect(copy).toBeVisible();
  const stableTable = page.locator(".message.response table");
  await expect(stableTable).toBeVisible();
  await copy.evaluate((node) => {
    window.__stableCopyControl = node;
  });
  await stableTable.evaluate((node) => {
    window.__stableMarkdownTable = node;
    window.__stableRemovalCount = 0;
    window.__maximumAnswerLength = 0;
    window.__stableRemovalObserver = new MutationObserver((records) => {
      window.__maximumAnswerLength = Math.max(
        window.__maximumAnswerLength,
        document.querySelector(".message.response > .markdown")?.textContent
          ?.length || 0,
      );
      for (const record of records)
        for (const removed of record.removedNodes)
          if (
            removed === window.__completedMarkdownBlock ||
            removed === window.__stableMarkdownTable ||
            removed === window.__stableCopyControl ||
            (removed instanceof Element &&
              (removed.contains(window.__completedMarkdownBlock) ||
                removed.contains(window.__stableMarkdownTable) ||
                removed.contains(window.__stableCopyControl)))
          )
            window.__stableRemovalCount++;
    });
    window.__stableRemovalObserver.observe(
      document.querySelector(".message.response > .markdown"),
      { childList: true, subtree: true },
    );
    window.__maximumAnswerLength = document.querySelector(
      ".message.response > .markdown",
    ).textContent.length;
  });

  await opening.evaluate((node) => {
    const selection = getSelection();
    const range = document.createRange();
    range.selectNodeContents(node);
    selection.removeAllRanges();
    selection.addRange(range);
  });
  const selected = await page.evaluate(() => getSelection().toString());

  const answer = page.locator(".message.response > .markdown");
  await copy.click();
  await expect
    .poll(() =>
      page.evaluate(() => ({
        connected: window.__stableCopyControl.isConnected,
        copied:
          window.__stableCopyControl.getAttribute("aria-label") === "Copied!",
        same:
          window.__stableCopyControl ===
          document.querySelector('.message.response [aria-label*="Cop"]'),
      })),
    )
    .toEqual({ connected: true, copied: true, same: true });
  await expect(page.locator(".composer .status-led.running")).toHaveCount(0, {
    timeout: 30_000,
  });
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  const maximumLength = await page.evaluate(() => window.__maximumAnswerLength);
  const finalSnapshot = await (
    await request.get(`/api/sessions/${session.id}`)
  ).json();
  const finalBlock = finalSnapshot.state.view.blocks.find(
    (block) => block.kind === "assistant",
  );
  await testInfo.attach("continuity-metadata.json", {
    body: JSON.stringify(
      {
        id: finalBlock?.id,
        response_id: finalBlock?.response_id,
        content_revision: finalBlock?.content_revision,
        content_complete: finalBlock?.content_complete,
        text_bytes: finalBlock?.text_bytes,
        retained_text_bytes: finalBlock?.retained_text_bytes,
        snapshot_text_length: finalBlock?.text?.length,
        maximum_dom_length: maximumLength,
      },
      null,
      2,
    ),
    contentType: "application/json",
  });
  expect(
    maximumLength,
    JSON.stringify({
      id: finalBlock?.id,
      response_id: finalBlock?.response_id,
      content_revision: finalBlock?.content_revision,
      content_complete: finalBlock?.content_complete,
      text_bytes: finalBlock?.text_bytes,
      retained_text_bytes: finalBlock?.retained_text_bytes,
      snapshot_text_length: finalBlock?.text?.length,
      maximum_dom_length: maximumLength,
    }),
  ).toBeGreaterThan(8000);
  await command("permissions", {
    session_id: session.id,
    generation: session.generation,
    mode: "ask",
  });
  await expect
    .poll(() =>
      page.evaluate(() => ({
        same:
          window.__completedMarkdownBlock ===
          [
            ...document.querySelectorAll(".message.response > .markdown p"),
          ].find((node) =>
            node.textContent?.includes("Stable opening paragraph"),
          )?.parentElement,
        selected: getSelection().toString(),
        thinking: document.querySelector(".thinking")?.open,
        copy:
          window.__stableCopyControl ===
          document.querySelector('.message.response [aria-label*="Cop"]'),
        table:
          window.__stableMarkdownTable ===
          document.querySelector(".message.response table"),
        length: document.querySelector(".message.response > .markdown")
          ?.textContent?.length,
        removals: window.__stableRemovalCount,
      })),
    )
    .toEqual({
      same: true,
      selected,
      thinking: true,
      copy: true,
      table: true,
      length: maximumLength,
      removals: 0,
    });
  await page.evaluate(() => window.__stableRemovalObserver.disconnect());
});

test("subagent tasks are readable and compaction never opens an unsolicited viewer", async ({
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
  await command("permissions", {
    session_id: session.id,
    generation: session.generation,
    mode: "yolo",
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await page.evaluate(() => {
    document.documentElement.style.setProperty("--safe-top", "47px");
    document.documentElement.style.setProperty("--safe-bottom", "34px");
  });
  await online(page);
  const composer = page.getByLabel("Message or guidance");
  await composer.fill("Delegate preview task");
  await composer.press("Enter");
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  // The first load is held, to see the sheet's layout while it loads, and
  // the refresh that follows it, to see it land on a follow-up being typed.
  // The refresh forces a retained older page, whose control shows that it
  // was applied and must share the thread's scroll.
  let releaseFirst, releaseRefresh;
  const held = [
    new Promise((resolve) => (releaseFirst = resolve)),
    new Promise((resolve) => (releaseRefresh = resolve)),
  ];
  let loads = 0;
  await page.route("**/api/command", async (route) => {
    const body = route.request().postDataJSON();
    if (body.kind !== "activity" || body.operation !== "inspect" || body.detail)
      return route.continue();
    const load = loads++;
    await held[load];
    const response = await route.fetch();
    const value = await response.json();
    if (load && value.result?.conversation) {
      value.result.conversation.more = true;
      value.result.conversation.before = 1;
    }
    await route.fulfill({ response, json: value });
  });
  // The subagent's own tool row opens it: the row names the task and keeps
  // a visible link to the agent instead of repeating its whole answer.
  const subagentRow = page
    .locator(".message.tool")
    .filter({ hasText: /Delegated .*Review the full task\./ });
  await expect(subagentRow).not.toContainText("resume with subagent");
  await subagentRow
    .getByRole("button", { name: "Open agent", exact: true })
    .click();
  // One sheet; its title follows the page shown in it.
  const detail = page.locator("dialog.activity-view");
  await expect(detail.locator(":scope > header > h2")).toHaveText("Subagent");
  // While the thread loads, every section already holds its final place:
  // the thread shows a skeleton and the guidance form sits where it stays.
  await expect(detail.getByText("Loading the thread…")).toBeVisible();
  const place = () =>
    Promise.all(
      [".child-thread", ".guidance-form"].map((selector) =>
        detail.locator(selector).boundingBox(),
      ),
    );
  const loadingPlace = await place();
  releaseFirst();
  await expect(detail.locator(".child-thread .message").first()).toBeVisible();
  expect(await place()).toEqual(loadingPlace);
  await expect(detail.getByText("Process output")).toHaveCount(0);
  await expect(
    detail.getByLabel("Estimated context", { exact: true }),
  ).toHaveText(/est\. ctx [\d.]+k?\/[\d.]+[kM]? · \d+% left/);
  await expect(detail.locator(".detail-label, .detail-task")).toHaveCount(0);
  const followUp = detail.getByRole("textbox", { name: "Follow-up" });
  await expect(followUp).toHaveCSS("border-top-style", "solid");
  const followUpNode = await followUp.elementHandle();
  await followUp.fill("Retained follow-up");
  await followUp.focus();
  const thread = detail.locator('[aria-label="Subagent task"]');
  const older = thread.getByRole("button", {
    name: "Load older retained messages",
    exact: true,
  });
  await expect(older).toHaveCount(0);
  releaseRefresh();
  await expect(older).toHaveCount(1);
  expect(await followUpNode?.evaluate((element) => element.isConnected)).toBe(
    true,
  );
  await expect(followUp).toHaveValue("Retained follow-up");
  await expect(followUp).toBeFocused();
  // Popup threads carry the same rows and viewers as chat history.
  await expect(thread.locator(".message")).not.toHaveCount(0);
  expect(
    await older.evaluate((button) => {
      const first = button.closest(".child-thread").querySelector(".message");
      return !!(
        button.compareDocumentPosition(first) & Node.DOCUMENT_POSITION_FOLLOWING
      );
    }),
  ).toBe(true);

  await expect(thread.locator(".tool-disclosure")).not.toHaveCount(0);
  const toolRow = thread.locator(".tool-disclosure").first();
  await toolRow.locator("summary").click();
  await toolRow
    .getByRole("button", { name: "Tool input/output", exact: true })
    .click();
  // A page inside the same sheet, never a second dialog; Back returns.
  const heading = detail.locator(":scope > header > h2");
  await expect(heading).toHaveText("Tool input/output");
  await expect(page.locator("dialog[open]")).toHaveCount(1);
  await detail.getByRole("button", { name: "Back", exact: true }).click();
  await expect(heading).toHaveText("Subagent");
  await detail
    .getByRole("button", { name: "Conversation statistics", exact: true })
    .click();
  const stats = detail;
  await expect(heading).toHaveText("Subagent statistics");
  await expect(stats.locator("dt").filter({ hasText: /^Cost$/ })).toBeVisible();
  await expect(
    stats.getByText("Parent turn time", { exact: true }),
  ).toBeVisible();
  await expect(stats.getByText("Model time", { exact: true })).toBeVisible();
  const statsBody = stats.locator(".activity-page");
  await expect(statsBody).toHaveCSS("overflow-y", "auto");
  await statsBody.hover();
  await page.mouse.wheel(0, 600);
  await expect
    .poll(() => statsBody.evaluate((node) => node.scrollTop))
    .toBeGreaterThan(0);
  await detail.getByRole("button", { name: "Back", exact: true }).click();
  await expect(detail).toHaveCSS("outline-style", "none");
  await expect(detail.locator(":scope > header > h2")).toHaveCSS(
    "outline-style",
    "none",
  );
  // Native close may fall back to the dialog when its previous opener is gone.
  await detail.evaluate((node) => node.focus());
  await expect(detail).toBeFocused();
  await expect(detail).toHaveCSS("outline-style", "none");
  await expect(detail.locator(":scope > header > h2")).toHaveCSS(
    "outline-style",
    "none",
  );
  await thread
    .getByRole("button", { name: /^Turn statistics: / })
    .last()
    .click();
  await expect(
    stats.getByText("Model at completion", { exact: true }),
  ).toBeVisible();
  await expect(stats.getByText("TTFT", { exact: true })).toBeVisible();
  await detail.getByRole("button", { name: "Back", exact: true }).click();
  await detail
    .getByRole("button", { name: "Model and effort", exact: true })
    .click();
  const childModel = page.getByRole("dialog", {
    name: "Model and effort",
    exact: true,
  });
  await childModel
    .getByRole("combobox", { name: "Model", exact: true })
    .fill("mock/main");
  await childModel.getByRole("option").filter({ hasText: "mock/main" }).click();
  await childModel.getByRole("button", { name: "Apply", exact: true }).click();
  const followUpRequest = page.waitForRequest((request) => {
    if (!request.url().endsWith("/api/command") || request.method() !== "POST")
      return false;
    const body = request.postDataJSON();
    return body.kind === "activity" && body.operation === "followup";
  });
  // Follow-up sends from the popup and receipts like the composer.
  await detail
    .getByRole("button", { name: "Start follow-up", exact: true })
    .click();
  expect((await followUpRequest).postDataJSON().model).toBe("mock/main");
  await expect(page.locator(".composer > form .model-selector")).toContainText(
    "mock/model-b",
  );
  await expect(detail.getByText("Follow-up started.")).toBeVisible({
    timeout: 20000,
  });
  await expect(
    thread.getByRole("heading", { name: "Verified response", exact: true }),
  ).toHaveCount(2);
  await expect(detail).not.toContainText("saved model is unavailable");
  await expect(detail.locator(".model-selector")).toContainText("mock/main");
  await detail
    .getByRole("button", { name: "Show full message", exact: true })
    .click();
  await expect(detail.locator('[aria-label="Subagent task"]')).toContainText(
    "TASK-END-MARKER",
  );
  expect(
    (await detail.locator('[aria-label="Subagent task"]').textContent()).length,
  ).toBeGreaterThan(16000);
  // Do not create empty metadata sections for ordinary agents.
  await expect(detail.getByText("System prompt", { exact: true })).toHaveCount(
    0,
  );
  await detail.getByText("Task and run details").click();
  await expect(detail.locator('section[aria-label="Run details"]')).toHaveCount(
    0,
  );
  await detail
    .getByRole("button", { name: "Close subagent", exact: true })
    .click();
  await expect(page.locator(".composer .activity-caption")).toHaveText("Ready");
  // A finished subagent stays reachable from the status line, like /agents.
  await page.getByRole("button", { name: "Activity", exact: true }).click();
  const idle = page.locator(".activity-sheet .activity-open");
  await expect(idle).toHaveCount(1);
  await expect(idle).toContainText("idle");
  await idle.click();
  await expect(detail.locator(":scope > header > h2")).toHaveText("Subagent");
  await detail
    .getByRole("button", { name: "Close subagent", exact: true })
    .click();
  await composer.fill("/compact");
  await composer.press("Enter");
  await expect(
    page
      .locator(".event-row > summary")
      .filter({ hasText: "Context compacted" }),
  ).toBeVisible();
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await page.getByRole("button", { name: "Raw context", exact: true }).click();
  const context = page.getByRole("dialog", {
    name: "Raw context",
    exact: true,
  });
  await expect(context).toContainText("COMPACT-PREVIEW-SUMMARY");
  await context
    .getByRole("button", { name: "Close raw context", exact: true })
    .click();
  await composer.fill("Background activity probe");
  await composer.press("Enter");
  // A finished background task reads like any tool row, its last output
  // lines visible without expanding.
  const receipt = page
    .locator(".message.tool")
    .filter({ hasText: "BROWSER_ACTIVITY" })
    .filter({ hasText: "Finished" });
  await expect(receipt).toBeVisible();
  await expect(receipt.locator("summary")).toContainText("Finished");
  await expect(
    receipt.getByRole("button", { name: "Tool input/output" }),
  ).toHaveCount(0);
  await page.reload();
  await expect(receipt).toBeVisible();
  await expect(receipt).toHaveCount(1);
});

test("denied browser storage keeps the conversation usable in memory", async ({
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
  await page.addInitScript(() => {
    for (const name of ["localStorage", "sessionStorage"])
      Object.defineProperty(globalThis, name, {
        get() {
          throw new DOMException("Storage access denied", "SecurityError");
        },
      });
  });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("A draft kept in memory");
  await expect(prompt).toHaveValue("A draft kept in memory");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.locator(".transcript")).toContainText(
    "A draft kept in memory",
  );
  await expect(page.locator(".transcript")).toContainText("Verified response");
  await expect(prompt).toHaveValue("");
});
