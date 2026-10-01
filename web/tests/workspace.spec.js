import { test, expect } from "./fixtures.js";
import { mkdir, writeFile } from "node:fs/promises";

test("appearance and configuration remain usable at large scales", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  // A phone opens settings on its section list.
  await page
    .locator(".settings-nav")
    .getByRole("button", { name: "General", exact: true })
    .click();
  await expect(page.getByLabel("Appearance")).toHaveValue("system");
  await page.emulateMedia({ colorScheme: "dark" });
  await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
  await page.emulateMedia({ colorScheme: "light" });
  await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
  await expect(page.locator('meta[name="theme-color"]')).toHaveAttribute(
    "content",
    "#ffffff",
  );
  const composer = page.getByLabel("Message or guidance");
  const originalText = await composer.evaluate(
    (element) => getComputedStyle(element).fontSize,
  );
  await expect(
    page.getByText("Scales the entire interface, conversation included"),
  ).toBeVisible();
  const interfaceText = await page
    .locator(".conversation-head h1")
    .evaluate((element) => getComputedStyle(element).fontSize);
  // Any text button scales with the interface; Back is always shown here.
  const textButton = page.getByRole("button", { name: "Back", exact: true });
  const normalControl = await textButton.evaluate((element) => {
    const style = getComputedStyle(element);
    return {
      height: element.getBoundingClientRect().height,
      padding: parseFloat(style.paddingInlineStart),
    };
  });
  await page.getByLabel("Zoom", { exact: true }).fill("50");
  await expect(page.locator("html")).toHaveCSS("--zoom", "0.5");
  await composer.focus();
  expect(
    await page.evaluate(() => ({
      scale: visualViewport?.scale,
      scrollX,
      scrollY,
    })),
  ).toEqual({ scale: 1, scrollX: 0, scrollY: 0 });
  const smallText = await composer.evaluate((element) =>
    parseFloat(getComputedStyle(element).fontSize),
  );
  expect(smallText).toBeLessThan(parseFloat(originalText) * 0.7);
  const smallControl = await textButton.evaluate((element) => {
    const style = getComputedStyle(element);
    return {
      height: element.getBoundingClientRect().height,
      padding: parseFloat(style.paddingInlineStart),
    };
  });
  expect(smallControl.height).toBeLessThan(normalControl.height * 0.7);
  expect(smallControl.padding).toBeLessThan(normalControl.padding * 0.7);
  await page.getByLabel("Zoom", { exact: true }).fill("110");
  await expect(page.locator("html")).toHaveCSS("--zoom", "1.1");
  // One dial moves type and spacing together: conversation and chrome
  // type both grow, unlike the composer staying put before.
  await expect(composer).not.toHaveCSS("font-size", originalText);
  await expect(page.locator(".conversation-head h1")).not.toHaveCSS(
    "font-size",
    interfaceText,
  );
  await page.getByLabel("Zoom", { exact: true }).fill("200");
  await expect(page.locator("html")).toHaveCSS("--zoom", "2");
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  await page.getByLabel("Zoom", { exact: true }).fill("100");
  await expect(page.locator("html")).toHaveCSS("--zoom", "1");
  await expect(composer).toHaveCSS("font-size", originalText);
  await page.getByRole("button", { name: "Back", exact: true }).click();
  await page
    .locator(".settings-nav")
    .getByRole("button", { name: "Advanced", exact: true })
    .click();
  // Advanced lists only what was changed or is locked; search finds the
  // rest by its plain name or its variable.
  const advanced = page.locator(".configuration");
  const changed = advanced.getByRole("button", { name: /^Steps per turn/ });
  await expect(changed).toHaveCount(0);
  await expect(advanced).toContainText("Everything else is at its default");
  const find = page.getByLabel("Find a setting");
  // A setting only a terminal uses is not offered here.
  await find.fill("UAGENT_MARKDOWN");
  await expect(advanced).toContainText("No setting matches.");
  await find.fill("UAGENT_MAX_STEPS");
  await changed.click();
  // One sheet edits it: what it is for, the value, why, and two ways out.
  const sheet = page.getByRole("dialog", { name: "Steps per turn" });
  await expect(sheet).toContainText("Default: 0");
  await expect(sheet).toContainText("applies from your next message");
  await sheet.getByRole("spinbutton").fill("23");
  await sheet.getByRole("button", { name: "Save" }).click();
  await expect(sheet).toHaveCount(0);
  await find.fill("");
  await expect(changed).toContainText("23");
  // Use default removes the change, and the row leaves the list.
  await changed.click();
  await sheet.getByRole("button", { name: "Use default" }).click();
  await expect(sheet).toHaveCount(0);
  await expect(changed).toHaveCount(0);

  // Reset all asks first, then returns every change to its default.
  await find.fill("steps per turn");
  await changed.click();
  await sheet.getByRole("spinbutton").fill("24");
  await sheet.getByRole("spinbutton").press("Enter");
  await expect(sheet).toHaveCount(0);
  await find.fill("");
  await expect(changed).toContainText("24");
  await page.getByRole("button", { name: /Reset all to defaults/ }).click();
  const confirm = page.getByRole("dialog", { name: "Reset all to defaults" });
  await expect(confirm).toContainText("API keys");
  await confirm.getByRole("button", { name: "Reset all", exact: true }).click();
  await expect(confirm).toHaveCount(0);
  await expect(changed).toHaveCount(0);
  await page.getByRole("button", { name: "Back", exact: true }).click();
  await page
    .locator(".settings-nav")
    .getByRole("button", { name: "General", exact: true })
    .click();
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
});

test("unread completions, background activity and conversation lifecycle", async ({
  page,
  context,
  host: fixture,
  session,
  command,
  request,
}) => {
  await command("rename", {
    session_id: session.id,
    generation: session.generation,
    title: "UI refactor proof",
  });
  await page.goto(`/#session=${session.id}`);
  await expect(page.locator(".conversation-head h1")).toHaveText(
    "UI refactor proof",
  );
  const model = page.getByRole("button", {
    name: "Model and effort",
    exact: true,
  });
  const conversationMenu = page
    .locator(".conversation-head")
    .getByLabel("Conversation menu", { exact: true });
  const originalHash = await page.evaluate(() => location.hash);
  await page
    .getByRole("complementary", { name: "Projects and sessions" })
    .getByRole("button", { name: "New conversation", exact: true })
    .click();
  await page.getByLabel("Directory on the host").fill(fixture.project);
  await page
    .getByRole("button", { name: "Start conversation", exact: true })
    .click();
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  await model.click();
  await page.getByLabel("Model", { exact: true }).selectOption("mock/model-b");
  await page.getByRole("button", { name: "Apply", exact: true }).click();
  await expect(model).toHaveText(/mock\/model-b/);
  const secondHash = await page.evaluate(() => location.hash);
  await page.getByLabel("Message or guidance").fill("Unread completion probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "Stop", exact: true }),
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
  await page.getByRole("button", { name: "Allow once", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "Activity", exact: true }),
  ).toContainText("1 command");
  // "Now" lists the running command; its row opens the inspector sheet.
  const now = page.getByRole("button", { name: "Activity", exact: true });
  await now.click();
  const activity = page
    .locator(".activity-row")
    .filter({ hasText: "BROWSER_ACTIVITY" });
  await activity.locator(".activity-open").click();
  await expect(page.getByRole("dialog").locator(".detail-command")).toHaveText(
    "printf BROWSER_ACTIVITY; sleep 10",
  );
  await page
    .getByRole("dialog")
    .getByRole("button", { name: /^Close / })
    .click();
  await now.click();
  await activity.getByRole("button", { name: /^Stop / }).click();
  await expect(page.locator(".composer .activity-toggle")).not.toContainText(
    "1 command",
  );
  // Finished work leaves "now"; its call's row stops reading as running.
  await expect(
    page.locator(".tool-disclosure").filter({ hasText: "BROWSER_ACTIVITY" }),
  ).not.toContainText("running");
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  // The activity sheet is modal: close it before reaching the page again.
  await page.keyboard.press("Escape");
  await conversationMenu.click();
  await page
    .locator(".conversation-head")
    .getByRole("menuitem", { name: "Fork conversation", exact: true })
    .click();
  await expect(page.locator(".conversation-head h1")).toHaveText(
    "Fork of Unread completion probe",
  );
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
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
  // Live sessions delete directly now: the dialog closes the worker first.
  await expect(
    page
      .locator(".conversation-head")
      .getByRole("menuitem", { name: "Delete", exact: true }),
  ).toBeEnabled();
  await page
    .locator(".conversation-head")
    .getByRole("menuitem", { name: "Delete", exact: true })
    .click();
  const deleter = page.getByRole("dialog", { name: "Delete conversation?" });
  await expect(deleter).toContainText("UI refactor proof");
  await expect(deleter).toContainText("closes first");
  await page
    .getByRole("button", { name: "Delete permanently", exact: true })
    .click();
  // Close-then-delete chains a worker shutdown: the dialog stays up
  // until the worker exits and the record is gone.
  await expect(
    page.getByRole("dialog", { name: "Delete conversation?" }),
  ).toBeHidden({ timeout: 20000 });
  await expect(page.locator(".conversation-head h1")).toHaveText(
    "Your workspace",
  );
  await expect(
    page.getByRole("button", { name: /UI refactor proof/ }),
  ).toHaveCount(0);
});

test("retained history stays bounded and merges overlapping pages once", async ({
  page,
  host: fixture,
}, testInfo) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await mkdir(`${fixture.home}/.uagent/history`, { recursive: true });
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
  // Older pages are history, not arrivals: nothing reads as new.
  await expect(page.locator(".jump")).not.toContainText("new");
  await page
    .getByRole("button", { name: "Jump to latest", exact: true })
    .click();
  await expect(page.locator(".message")).toHaveCount(64);
  await expect(page.locator(".message").last()).toContainText(
    "Retained message 1999.",
  );
});

test("load-older holds position, spins, and keeps the newest tail", async ({
  page,
  host: fixture,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await mkdir(`${fixture.home}/.uagent/history`, { recursive: true });
  const messages = Array.from({ length: 300 }, (_, index) => ({
    role: index % 2 ? "assistant" : "user",
    content: `Retained message ${index}. ` + "History anchor. ".repeat(20),
  }));
  await writeFile(
    `${fixture.home}/.uagent/history/large.json`,
    JSON.stringify({
      format: 3,
      cwd: fixture.project,
      model: "test",
      session_id: "anchor-history",
      title: "Anchor history",
      turns: 150,
    }) +
      "\n" +
      JSON.stringify({
        messages,
        message_kinds: messages.map((item) => item.role),
        archive: [],
        archive_dropped_segments: 0,
        context_tokens: 0,
        usage: {},
        tool_displays: {},
      }),
    { mode: 0o600 },
  );
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await page.getByRole("button", { name: /Anchor history/ }).click();
  const older = page.getByRole("button", {
    name: "Load older retained messages",
    exact: true,
  });
  await expect(older).toBeVisible();
  await page.locator(".transcript").evaluate((element) => {
    element.scrollTop = 0;
  });
  const newest = await page
    .locator(".message")
    .last()
    .getAttribute("data-message-id");
  const first = await page.locator(".transcript").evaluate((element) => {
    const top = element.scrollTop;
    const rows = [...element.querySelectorAll("article.message")];
    const row = rows.find((item) => item.offsetTop + item.offsetHeight > top);
    return row
      ? {
          id: row.getAttribute("data-message-id"),
          offset: row.offsetTop - top,
        }
      : null;
  });
  const firstId = first?.id;
  const firstOffset = first?.offset || 0;
  // Gate the older page: the button must go stateful while loading.
  let releasePage;
  const pageGate = new Promise((resolve) => (releasePage = resolve));
  const olderRoute = async (route) => {
    const response = await route.fetch();
    await pageGate;
    await route.fulfill({ response });
  };
  await page.route("**/api/sessions/*?before=*", olderRoute);
  await older.click();
  const loading = page.getByRole("button", {
    name: "Loading older messages…",
  });
  await expect(loading).toBeVisible();
  await expect(loading).toBeDisabled();
  releasePage();
  await expect
    .poll(
      () =>
        page
          .locator(".transcript")
          .evaluate(
            (element) =>
              element.scrollHeight - element.scrollTop - element.clientHeight,
          ),
      { timeout: 15000 },
    )
    .toBeGreaterThan(100);
  // Still reading history: not pinned to the bottom, the same first
  // message stays under the reader, and the newest tail survived.
  const gap = await page
    .locator(".transcript")
    .evaluate(
      (element) =>
        element.scrollHeight - element.scrollTop - element.clientHeight,
    );
  expect(gap).toBeGreaterThan(100);
  const held = await page
    .locator(".transcript")
    .evaluate((element, anchorId) => {
      const top = element.scrollTop;
      const anchor = [...element.querySelectorAll("article.message")].find(
        (row) => row.getAttribute("data-message-id") === anchorId,
      );
      return anchor
        ? {
            id: anchor.getAttribute("data-message-id"),
            drift: anchor.offsetTop - top,
          }
        : null;
    }, firstId);
  expect(held?.id).toBe(firstId);
  // The prepended page lands above the anchor: the previously first row
  // (plus part of its older neighbour) is visible, so "first visible"
  // cannot stay identical. What must hold is the anchor row itself staying
  // at the same offset under the reader.
  expect(Math.abs((held?.drift || 0) - firstOffset)).toBeLessThan(4);
  await expect(page.locator(".message").last()).toHaveAttribute(
    "data-message-id",
    newest || "",
  );
  await page.unroute("**/api/sessions/*?before=*", olderRoute);
});

// A value set by the environment is shown locked; a saved change that needs
// a restart offers to restart the running conversations.
test("locked settings and restart to apply", async ({ page, session }) => {
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await page
    .locator(".settings-nav")
    .getByRole("button", { name: "Advanced", exact: true })
    .click();
  // A locked setting is listed, with its value and no way to edit it.
  const advanced = page.locator(".configuration");
  const locked = advanced.getByRole("button", { name: /^Context window/ });
  await expect(locked).toContainText("Locked");
  await locked.click();
  const context = page.getByRole("dialog", { name: "Context window" });
  await expect(context).toContainText("Set by the environment");
  await expect(context.getByRole("spinbutton")).toHaveCount(0);
  await expect(context.getByRole("button", { name: "Save" })).toHaveCount(0);
  await page.keyboard.press("Escape");

  // A setting that needs a restart offers one, once, for what is running.
  const find = page.getByLabel("Find a setting");
  await find.fill("mcp call timeout");
  await advanced.getByRole("button", { name: /^MCP call timeout/ }).click();
  const timeout = page.getByRole("dialog", { name: "MCP call timeout" });
  await expect(timeout).toContainText("applies after a restart");
  await timeout.getByRole("spinbutton").fill("90");
  await timeout.getByRole("button", { name: "Save" }).click();
  const notice = page.getByRole("region", { name: "Restart to apply" });
  await expect(notice).toContainText("MCP call timeout");
  await notice
    .getByRole("button", { name: /^Restart 1 running conversation/ })
    .click();
  await expect(notice).toContainText("Restarted 1 conversation");
});
