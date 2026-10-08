import { test, expect, online } from "./fixtures.js";
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
    .getByRole("button", { name: "This browser", exact: true })
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
    .getByRole("button", { name: "All conversations", exact: true })
    .click();
  // Every setting is listed under its group; the box narrows them by plain
  // name or by variable.
  const form = page.locator(".configuration");
  await expect(
    form.getByRole("region", { name: "Models and connection" }),
  ).toBeVisible();
  const find = page.getByLabel("Find a setting");
  // A setting only a terminal uses is listed too, and says who reads it.
  await find.fill("UAGENT_MARKDOWN");
  await expect(form).toContainText("read by the terminal");
  await find.fill("no such setting");
  await expect(form).toContainText("No setting matches.");
  await find.fill("UAGENT_MAX_STEPS");
  // It is edited where it is listed, and saved on Enter or on leaving the
  // field: nothing half-typed is.
  const steps = form.getByRole("spinbutton", { name: "Steps per turn" });
  const reset = form.getByRole("button", { name: "Reset Steps per turn" });
  await expect(reset).toHaveCount(0);
  await steps.fill("2");
  await expect(reset).toHaveCount(0);
  await steps.fill("23");
  await steps.press("Enter");
  await expect(reset).toBeVisible();
  // What the host refuses stays in the field, with why.
  await steps.fill("-4");
  await steps.press("Enter");
  await expect(form.getByRole("alert")).toContainText("UAGENT_MAX_STEPS");
  await expect(steps).toHaveValue("-4");
  // Escape takes back what was typed.
  await steps.press("Escape");
  await expect(steps).toHaveValue("23");
  // Putting it back is its own action.
  await reset.click();
  await expect(reset).toHaveCount(0);
  await expect(steps).toHaveValue("");

  // Reset all asks first, then returns every change to its default.
  await steps.fill("24");
  await steps.blur();
  await expect(reset).toBeVisible();
  await find.fill("");
  await page.getByRole("button", { name: /Reset all to defaults/ }).click();
  const confirm = page.getByRole("dialog", { name: "Reset all to defaults" });
  await expect(confirm).toContainText("API keys");
  await confirm.getByRole("button", { name: "Reset all", exact: true }).click();
  await expect(confirm).toHaveCount(0);
  await expect(reset).toHaveCount(0);
  await page.getByRole("button", { name: "Back", exact: true }).click();
  await page
    .locator(".settings-nav")
    .getByRole("button", { name: "This browser", exact: true })
    .click();
  await page.getByLabel("Appearance").selectOption("light");
  await page.emulateMedia({ colorScheme: "dark" });
  await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
  // Animations follow the device until Settings turns them off.
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await expect(page.locator("html")).toHaveAttribute("data-motion", "on");
  await page.getByLabel("Animations").selectOption("off");
  await expect(page.locator("html")).toHaveAttribute("data-motion", "off");
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
  // The popover's history entry leaves in a step of its own, and a
  // navigation made before that step lands is undone by it.
  await expect
    .poll(() => page.evaluate(() => history.state?.layer ?? 0))
    .toBe(0);

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
  const older = page.getByRole("button", {
    name: "Load older retained messages",
    exact: true,
  });
  // Refresh reconnected the event stream, and a scripted click does not
  // wait for the button that disables meanwhile.
  await expect(older).toBeEnabled();
  await older.evaluate((element) => {
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
    .poll(() =>
      page
        .locator(".transcript")
        .evaluate(
          (element) =>
            element.scrollHeight - element.scrollTop - element.clientHeight,
        ),
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

// Each kind of control saves in its own way, a project's value overrides
// what is saved for all, and what is being typed survives everyone else's
// saves.
test("a project overrides a setting; a draft survives other saves", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  const nav = page.locator(".settings-nav");
  const form = page.locator(".configuration");
  const field = (name) => form.locator(`[data-setting="${name}"]`);
  await nav.getByRole("button", { name: /^This project/ }).click();
  await expect(form).toContainText("Remove all overrides");
  // A number is saved on leaving its field.
  const steps = form.getByRole("spinbutton", { name: "Steps per turn" });
  await steps.fill("5");
  await steps.blur();
  await expect(field("UAGENT_MAX_STEPS")).toContainText(
    "Overrides 0 from all conversations",
  );
  // A choice applies as it is chosen, and something typed elsewhere but not
  // yet saved is still there after it.
  const calls = form.getByRole("spinbutton", { name: "Tool calls per turn" });
  await calls.fill("77");
  // A mode is named as it is everywhere else, and an effort is chosen, not
  // typed.
  const approval = form.getByRole("combobox", { name: "Approval mode" });
  await expect(
    approval.getByRole("option", { name: "Auto review" }),
  ).toHaveCount(1);
  await expect(
    form
      .getByRole("combobox", { name: "Reasoning effort" })
      .getByRole("option", { name: "high", exact: true }),
  ).toHaveCount(1);
  await approval.selectOption("auto");
  await expect(
    form.getByRole("button", { name: "Reset Approval mode" }),
  ).toBeVisible();
  await expect(calls).toHaveValue("77");
  await expect(
    form.getByRole("button", { name: "Reset Tool calls per turn" }),
  ).toHaveCount(0);
  await calls.press("Escape");

  // What is saved for all shows that this project decides instead.
  await nav.getByRole("button", { name: "All conversations" }).click();
  await expect(field("UAGENT_MAX_STEPS")).toContainText(
    "Overridden in this project",
  );
  // A switch applies as it flips; putting it back is the reset beside it.
  const memory = form.getByRole("switch", { name: "Memory", exact: true });
  await expect(memory).toBeChecked();
  await memory.uncheck();
  const restore = form.getByRole("button", { name: "Reset Memory" });
  await restore.click();
  await expect(memory).toBeChecked();
  await expect(restore).toHaveCount(0);

  // Removing the project's overrides leaves what is saved for all.
  await nav.getByRole("button", { name: /^This project/ }).click();
  await form.getByRole("button", { name: /Remove all overrides/ }).click();
  await page
    .getByRole("dialog", { name: "Remove all overrides" })
    .getByRole("button", { name: "Remove all", exact: true })
    .click();
  await expect(
    form.getByRole("button", { name: "Reset Steps per turn" }),
  ).toHaveCount(0);
});

// A value set by the environment is shown locked; a saved change that needs
// a restart offers to restart the running conversations.
test("locked settings and restart to apply", async ({ page, session }) => {
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await page
    .locator(".settings-nav")
    .getByRole("button", { name: "All conversations", exact: true })
    .click();
  // A locked setting is listed, with its value and no way to edit it.
  const form = page.locator(".configuration");
  const locked = form.locator('[data-setting="UAGENT_CONTEXT"]');
  await expect(locked).toContainText(
    "Set by environment variable UAGENT_CONTEXT — change it there",
  );
  await expect(locked.getByRole("spinbutton")).toHaveCount(0);

  // A setting that needs a restart offers one, once, for what is running.
  const timeout = form.getByRole("spinbutton", { name: "MCP call timeout" });
  await timeout.fill("90");
  await timeout.press("Enter");
  const notice = page.getByRole("region", { name: "Restart to apply" });
  await expect(notice).toContainText("MCP call timeout");
  await notice
    .getByRole("button", { name: /^Restart 1 running conversation/ })
    .click();
  await expect(notice).toContainText("Restarted 1 conversation");
});

// What the terminal does with /share and /restart, from the conversation
// menu; /quit detaches a terminal and so closes nothing here.
test("the conversation menu exports and restarts; /quit closes nothing", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const generation = () =>
    page.evaluate(
      (id) =>
        fetch(`/api/sessions/${id}`)
          .then((response) => response.json())
          .then((value) => value.metadata.generation),
      session.id,
    );
  const composer = page.getByLabel("Message or guidance");
  await composer.fill("/quit");
  await composer.press("Enter");
  await expect(
    page.getByText("Use Close session in the conversation menu."),
  ).toBeVisible();
  expect(await generation()).toBe(session.generation);
  // The notice that follows replaces this error in the one banner.

  const head = page.locator(".conversation-head");
  const menu = head.getByLabel("Conversation menu", { exact: true });
  const item = (name) => head.getByRole("menuitem", { name, exact: true });
  await menu.click();
  await expect(item("Compact")).toBeVisible();
  await item("Export transcript").click();
  await expect(page.getByText(/^Transcript saved to .+\.md$/)).toBeVisible();
  await menu.click();
  await item("Restart").click();
  await expect(page.getByText("Conversation restarted.")).toBeVisible();
  await expect.poll(generation).not.toBe(session.generation);
});
