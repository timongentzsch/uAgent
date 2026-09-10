import { test, expect } from "./fixtures.js";
import { readFile } from "node:fs/promises";

test.use({ paired: false });

test("native host: mobile decisions, safe rendering, offline shell and private caching", async ({
  page,
  context,
  host: fixture,
}) => {
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
  await expect(
    page.getByRole("dialog", { name: "Model and effort" }),
  ).toHaveCount(0);
  await expect(model).toHaveText("mock/model-a:floor:high");
  await model.click();
  await page
    .getByRole("combobox", { name: "Model", exact: true })
    .selectOption({ label: "mock/model-b" });
  await page.getByLabel("Effort", { exact: true }).selectOption("high");
  await page.getByLabel("Variant", { exact: true }).selectOption("floor");
  await expect(page.getByLabel("Model", { exact: true })).toHaveValue(
    "mock/model-b",
  );
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
  const reply = page.locator(".message").filter({
    has: page.getByRole("heading", { name: "Verified response" }),
  });
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
    .getByRole("button", {
      name: "Close conversation statistics",
      exact: true,
    })
    .click();
  await expect(page.locator(".katex").first()).toBeVisible();
  await expect(page.locator(".hljs-built_in")).toContainText("print");
  await expect(page.locator(".markdown")).toContainText(["Prices $5 and $10."]);
  expect(await page.locator('a[href^="javascript:"]').count()).toBe(0);
  expect(await page.locator('img[src^="https:"]').count()).toBe(0);
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
  const answers = page.getByRole("heading", {
    name: "Verified response",
    exact: true,
  });
  let answerCount = await answers.count();
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
  await expect(answers).toHaveCount(answerCount + 1);
  await expect(page.locator(".status")).toHaveText("idle");
  answerCount = await answers.count();
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
  await expect(answers).toHaveCount(answerCount + 1);
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
});
