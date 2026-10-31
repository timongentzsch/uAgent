import {
  test,
  expect,
  online,
  withoutServiceWorker,
  selectProbeModel,
  chooseDetail,
  openRow,
  openRawContext,
} from "./fixtures.js";

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
        nodes: [
          {
            id: "m-1",
            parent: null,
            kind: "message",
            role: "assistant",
            body: { kind: "markdown", text: metadata.title, truncated: true },
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
  const { promise: held, resolve: release } = Promise.withResolvers();
  const { promise: reached, resolve: requested } = Promise.withResolvers();
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
    name: "Conversation statistics",
    exact: true,
  });
  await expect(context).toHaveAttribute("title", /Estimated context: 9,000 /);
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
  await expect(context).toHaveAttribute("title", /Estimated context: 9,000 /);
  // A repeated/out-of-order event must not roll back the same generation.
  await send(0, sessions[1], 3000, 11);
  await expect(context).toHaveAttribute("title", /Estimated context: 9,000 /);
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect
    .poll(() => page.evaluate(() => globalThis.testStreams.length))
    .toBe(2);
  await ready(1, 12);
  await send(1, sessions[1], 12000, 13);
  await expect(context).toHaveAttribute("title", /Estimated context: 12,000 /);
  await send(0, sessions[1], 800000, 99);
  await page.evaluate(() =>
    globalThis.testStreams[0].onerror(new Event("error")),
  );
  await expect(context).toHaveAttribute("title", /Estimated context: 12,000 /);
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await expect(page.locator(".error-banner")).toHaveCount(0);
});

test("tool rows and memory receipts survive reload and mobile rotation", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Exploration probe");
  await prompt.press("Enter");
  // The host grouped the two reads: the turn's work opens on one row under
  // their label, as a terminal shows them.
  const rows = page.locator(".transcript .node-row");
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  // The reply's thinking is a step of the work too.
  await expect(rows).toHaveText([
    /^Worked.*2 steps/,
    /^Explored.*2 calls/,
    /^Thinking/,
  ]);
  // The conversation menu sets how deep the tree opens: every call at full,
  // the whole turn's work as one line at minimal.
  await chooseDetail(page, "Full");
  await expect(rows).toHaveCount(5);
  await chooseDetail(page, "Minimal");
  await expect(rows).toHaveText([/^Worked.*2 steps/]);
  await chooseDetail(page, "Default");
  await expect(rows).toHaveCount(3);
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
  const title = page.locator(".node-row .node-headline").first();
  await title.evaluate((element) => {
    element.textContent = "long-command-".repeat(200);
  });
  const row = await title.locator("..").boundingBox();
  expect(row.height).toBeLessThan(60);
  expect(
    await page.evaluate(() => document.documentElement.scrollWidth),
  ).toBeLessThanOrEqual(391);
  await expect(page.locator(".composer .activity-button")).toHaveCount(0);
  await prompt.fill("Memory receipt probe: please save this test memory");
  await prompt.press("Enter");
  // The turn ends first: a reply arriving moves the row under a click on it.
  await expect(page.locator(".turn-footer")).toHaveCount(2);
  await expect(page.locator(".composer .status-led.running")).toHaveCount(0);
  // The receipt opens to a link to the memory.
  await openRow(page, "Saved memory project/browser-proof");
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
  await expect(page.locator(".composer .activity-button")).toHaveCount(0);
  await page.locator(".transcript").evaluate((element) => {
    element.scrollTop = 0;
  });
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeVisible();
  await page.reload();
  // Retained history replays through the live pipeline: the same group and
  // the receipt, none still running, and the row opened here stays open.
  await expect(
    page.locator(".node-row", { hasText: "Explored" }),
  ).toBeVisible();
  await expect(page.locator(".transcript .node.running")).toHaveCount(0);
  await expect(
    page.locator(".node-row", {
      hasText: "Saved memory project/browser-proof",
    }),
  ).toHaveAttribute("aria-expanded", "true");
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
  await selectProbeModel(command, session);
  await page.goto(`/#session=${session.id}`);
  await expect(page).toHaveURL(new RegExp(session.id));
  await expect(page.locator(".conversation-head h1")).toBeVisible();
  const prompt = page.getByLabel("Message or guidance");
  await expect(prompt).toBeVisible();
  await prompt.fill("Long continuity probe");
  await prompt.press("Enter");
  await expect(page.locator(".composer .status-led.running")).toBeVisible();

  // The reply's thinking is a row of the turn's work; a never-opened row
  // draws no body.
  const thinkingRow = page.locator(".node-row", { hasText: "Thinking" });
  await expect(thinkingRow).toBeVisible();
  const thinking = thinkingRow.locator("..");
  await expect(thinking.locator(".node-body")).toHaveCount(0);
  // Dispatched: the reply streaming moves the page under a real click, and
  // whether a click lands is not what this holds to.
  await thinkingRow.dispatchEvent("click");
  await expect(thinking.locator(".node-text")).toBeVisible();

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
    // How much of the answer shows: all of it, or only its settled blocks.
    // The block still streaming can show raw syntax for a moment (a
    // reference before its definition, a half-written marker), so only the
    // blocks before it are held to the end. A reference counts as its label.
    window.__answerLength = (settled = false) => {
      const blocks = [
        ...(document.querySelector(".message.response > .markdown")?.children ||
          []),
      ];
      return (settled ? blocks.slice(0, -1) : blocks)
        .map((block) => block.textContent || "")
        .join("")
        .replace(/\[([^\]]*)\]\[[^\]]*\]/g, "$1").length;
    };
    window.__stableMarkdownTable = node;
    window.__stableRemovalCount = 0;
    window.__maximumAnswerLength = 0;
    window.__stableRemovalObserver = new MutationObserver((records) => {
      window.__maximumAnswerLength = Math.max(
        window.__maximumAnswerLength,
        window.__answerLength(true),
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
    window.__maximumAnswerLength = window.__answerLength(true);
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
  await copy.dispatchEvent("click");
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
  await expect(page.locator(".composer .activity-button")).toHaveCount(0);
  const maximumLength = await page.evaluate(() => window.__maximumAnswerLength);
  const finalSnapshot = await (
    await request.get(`/api/sessions/${session.id}`)
  ).json();
  const finalNode = finalSnapshot.state.view.nodes.find(
    (node) => node.role === "assistant",
  );
  const continuity = {
    id: finalNode?.id,
    truncated: finalNode?.body?.truncated,
    snapshot_text_length: finalNode?.body?.text?.length,
    maximum_dom_length: maximumLength,
  };
  await testInfo.attach("continuity-metadata.json", {
    body: JSON.stringify(continuity, null, 2),
    contentType: "application/json",
  });
  expect(maximumLength, JSON.stringify(continuity)).toBeGreaterThan(8000);
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
        thinking: [...document.querySelectorAll(".node-row")]
          .find((row) => row.textContent?.includes("Thinking"))
          ?.getAttribute("aria-expanded"),
        copy:
          window.__stableCopyControl ===
          document.querySelector('.message.response [aria-label*="Cop"]'),
        table:
          window.__stableMarkdownTable ===
          document.querySelector(".message.response table"),
        // Nothing that settled while streaming is gone at the end.
        kept: window.__answerLength() >= window.__maximumAnswerLength,
        removals: window.__stableRemovalCount,
      })),
    )
    .toEqual({
      same: true,
      selected,
      thinking: "true",
      copy: true,
      table: true,
      kept: true,
      removals: 0,
    });
  await page.evaluate(() => window.__stableRemovalObserver.disconnect());
});

test("compaction never opens an unsolicited viewer", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const composer = page.getByLabel("Message or guidance");
  await composer.fill("Compaction probe");
  await composer.press("Enter");
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  await expect(page.locator(".composer .activity-button")).toHaveCount(0);
  await composer.fill("/compact");
  await composer.press("Enter");
  await expect(
    page.locator(".node-row").filter({ hasText: "Compacted" }),
  ).toBeVisible();
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await openRawContext(page);
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
  // A command left running in the background keeps its own row, which
  // takes the result when the command ends: one row, after a reload too.
  const row = page.locator(".node.ok > .node-row", {
    hasText: "BROWSER_ACTIVITY",
  });
  await expect(row).toBeVisible({ timeout: 20_000 });
  await page.reload();
  await expect(row).toBeVisible();
  await expect(row).toHaveCount(1);
});

test("denied browser storage keeps the conversation usable in memory", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session);
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
