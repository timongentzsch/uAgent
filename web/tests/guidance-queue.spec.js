import { test, expect, online } from "./fixtures.js";

test.beforeEach(({ command, session }) =>
  command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  }),
);

async function start(page, session) {
  await page.goto(`/#session=${session.id}`);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Queue controls probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.locator(".message.response")).toContainText(
    "Waiting for guidance",
  );
  return prompt;
}

for (const width of [320, 390, 1280]) {
  test(`queued messages stay with the composer until inserted (${width})`, async ({
    page,
    session,
  }, testInfo) => {
    await page.setViewportSize({ width, height: 844 });
    if (width === 390) await page.emulateMedia({ colorScheme: "dark" });
    const prompt = await start(page, session);
    const text =
      "You can also use the browser to verify the current kernel development status.";
    await prompt.fill(text);
    await page.getByRole("button", { name: "Queue next" }).click();
    const tray = page.getByRole("region", { name: "Queued messages" });
    await expect(tray).toContainText(text);
    await expect(tray).toContainText("After this turn");
    await expect(page.locator(".message.user", { hasText: text })).toHaveCount(
      0,
    );
    await page.reload();
    await online(page);
    await expect(tray).toContainText(text);
    await page.locator(".transcript").evaluate((node) => {
      node.scrollTop = 0;
    });
    const boxes = await page.evaluate(() => {
      const box = (selector) => {
        const { x, y, width, height, bottom } = document
          .querySelector(selector)
          .getBoundingClientRect();
        return { x, y, width, height, bottom };
      };
      return {
        tray: box(".guidance-queue"),
        text: box(".queue-text"),
        controls: box(".queue-controls"),
        input: box("#prompt"),
        overflow: document.documentElement.scrollWidth > innerWidth,
        buttons: [...document.querySelectorAll(".queue-controls button")].map(
          (node) => node.getBoundingClientRect().height,
        ),
      };
    });
    expect(boxes.overflow).toBe(false);
    expect(boxes.text.width).toBeGreaterThan(boxes.tray.width - 40);
    expect(boxes.controls.y).toBeGreaterThanOrEqual(boxes.text.bottom);
    expect(boxes.tray.bottom).toBeLessThanOrEqual(boxes.input.y);
    expect(boxes.tray.y).toBeGreaterThan(0);
    if (width < 600)
      for (const height of boxes.buttons)
        expect(height).toBeGreaterThanOrEqual(44);
    await page.screenshot({ path: testInfo.outputPath("queued.png") });
    if (width < 600) {
      await prompt.focus();
      await page.evaluate(() => {
        for (const [key, value] of Object.entries({
          height: 390,
          offsetTop: 70,
        }))
          Object.defineProperty(visualViewport, key, {
            configurable: true,
            value,
          });
        visualViewport.dispatchEvent(new Event("resize"));
        visualViewport.dispatchEvent(new Event("scroll"));
      });
      await expect
        .poll(async () =>
          Math.round((await page.locator("#app").boundingBox()).height),
        )
        .toBe(390);
      const queueBox = await tray.boundingBox();
      const composerBox = await page.locator(".composer").boundingBox();
      expect(queueBox.y).toBeGreaterThanOrEqual(70);
      expect(composerBox.y + composerBox.height).toBeLessThanOrEqual(461);
      await expect(prompt).toBeFocused();
      await page.screenshot({
        path: testInfo.outputPath("queued-keyboard.png"),
      });
    }
    await tray.getByRole("button", { name: "Insert now" }).click();
    await expect(tray).toHaveCount(0);
    await expect(page.locator(".message.user", { hasText: text })).toHaveCount(
      1,
    );
    await expect(
      page.getByRole("heading", { name: "Verified response", exact: true }),
    ).toBeVisible();
    await expect(
      page.getByRole("button", { name: "Continue", exact: true }),
    ).toHaveCount(0);
    await expect(
      page.getByRole("button", { name: /^Turn statistics:/ }),
    ).toHaveCount(1);
  });
}

test("queued guidance can be edited with its attachments without replacing a draft", async ({
  page,
  session,
}) => {
  const prompt = await start(page, session);
  await page.locator('.composer input[type="file"]').setInputFiles({
    name: "note.txt",
    mimeType: "text/plain",
    buffer: Buffer.from("source evidence"),
  });
  await expect(
    page.getByRole("button", { name: "Remove note.txt" }),
  ).toBeEnabled();
  await prompt.fill("Use the attached source");
  await page
    .getByRole("button", { name: "Send guidance", exact: true })
    .click();
  const tray = page.getByRole("region", { name: "Queued messages" });
  await expect(tray).toContainText("Next checkpoint");
  await expect(tray).toContainText("note.txt");
  await prompt.fill("Existing draft");
  await tray.getByRole("button", { name: "Edit", exact: true }).click();
  await expect(tray).toHaveCount(0);
  await expect(prompt).toHaveValue("Existing draft\nUse the attached source");
  await expect(
    page.getByRole("button", { name: "Remove note.txt" }),
  ).toBeVisible();
});

test("a recall that loses to delivery leaves the current draft alone", async ({
  page,
  session,
}) => {
  const prompt = await start(page, session);
  await prompt.fill("Already delivered guidance");
  await page
    .getByRole("button", { name: "Send guidance", exact: true })
    .click();
  const tray = page.getByRole("region", { name: "Queued messages" });
  await expect(tray).toContainText("Already delivered guidance");
  await prompt.fill("Keep my draft");
  await page.route("**/api/command", (route) => {
    const body = route.request().postDataJSON();
    if (body.kind !== "recall") return route.continue();
    return route.fulfill({
      contentType: "application/json",
      status: 409,
      body: JSON.stringify({
        request_id: body.request_id,
        accepted: false,
        error: "already delivered",
      }),
    });
  });
  await tray.getByRole("button", { name: "Edit", exact: true }).click();
  await expect(tray).toHaveCount(0);
  await expect(prompt).toHaveValue("Keep my draft");
});

test("a refused insertion keeps the queued message available", async ({
  page,
  session,
}) => {
  const prompt = await start(page, session);
  await prompt.fill("Keep this queued");
  await page.getByRole("button", { name: "Queue next" }).click();
  const tray = page.getByRole("region", { name: "Queued messages" });
  await expect(tray).toContainText("Keep this queued");
  await page.route("**/api/command", (route) => {
    const body = route.request().postDataJSON();
    if (body.kind !== "insert") return route.continue();
    return route.fulfill({
      contentType: "application/json",
      status: 409,
      body: JSON.stringify({
        request_id: body.request_id,
        accepted: false,
        error: "Insertion refused for test",
      }),
    });
  });
  await tray.getByRole("button", { name: "Insert now" }).click();
  await expect(page.locator(".error-banner")).toContainText(
    "Insertion refused for test",
  );
  await expect(tray).toContainText("Keep this queued");
  await expect(
    tray.getByRole("button", { name: "Edit", exact: true }),
  ).toBeEnabled();
});
