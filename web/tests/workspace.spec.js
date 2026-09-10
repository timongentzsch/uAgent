import { test, expect } from "./fixtures.js";
import { mkdir, writeFile } from "node:fs/promises";

test("appearance and configuration remain usable at large scales", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await expect(page.locator(".status")).toHaveText("idle");
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
  await page
    .getByRole("button", { name: "Jump to latest", exact: true })
    .click();
  await expect(page.locator(".message")).toHaveCount(64);
  await expect(page.locator(".message").last()).toContainText(
    "Retained message 1999.",
  );
});
