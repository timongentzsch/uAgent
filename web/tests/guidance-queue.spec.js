// @webkit
import { test, expect, online, selectProbeModel } from "./fixtures.js";

test.beforeEach(({ command, session }) => selectProbeModel(command, session));

async function start(page, session) {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Queue controls probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.locator(".message.response")).toContainText(
    "Waiting for guidance",
  );
  return prompt;
}

// Queues `text` with Queue next, or as guidance with Enter, and keeps its
// send from the host: the row is queued, the host has nothing. Returns the
// send's body.
async function queueUnsent(page, prompt, text, how) {
  let held;
  await page.route("**/api/command", (route) => {
    if (route.request().postDataJSON().kind !== "steer")
      return route.continue();
    held = route;
  });
  await prompt.fill(text);
  if (how === "Enter") await prompt.press("Enter");
  else await page.getByRole("button", { name: how }).click();
  await expect(
    page.getByRole("region", { name: "Queued messages" }),
  ).toContainText(text);
  await expect.poll(() => !!held).toBe(true);
  const body = held.request().postData();
  await held.abort();
  await page.unroute("**/api/command");
  return body;
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
    // Once drawn, the queued intent is durable even on an immediate reload.
    expect(
      await page.evaluate(
        (text) =>
          JSON.parse(sessionStorage.getItem("uagent-outgoing") || "[]").find(
            (item) => item.body?.text === text,
          )?.status,
        text,
      ),
    ).toBe("Queued for after this turn");
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
    await expect(page.locator(".turn-footer")).toHaveCount(1);
  });
}

// The host takes the command, and its answer never reaches the page whole.
for (const [how, lose] of [
  ["dropped", (route) => route.abort()],
  ["cut short", (route) => route.fulfill({ status: 200, body: "" })],
])
  test(`a queued message whose answer is ${how} stays queued across a reload`, async ({
    page,
    session,
  }) => {
    const prompt = await start(page, session);
    await page.route("**/api/command", async (route) => {
      await route.fetch();
      await lose(route);
    });
    const text = "Queued as the connection drops.";
    await prompt.fill(text);
    const lost = page.waitForEvent("requestfinished", (request) =>
      request.url().endsWith("/api/command"),
    );
    const failed = page.waitForEvent("requestfailed");
    await page.getByRole("button", { name: "Queue next" }).click();
    await Promise.race([lost, failed]);
    await page.unroute("**/api/command");
    const tray = page.getByRole("region", { name: "Queued messages" });
    await expect(tray).toContainText(text);
    await page.reload();
    await online(page);
    await expect(tray).toContainText(text);
    await expect(page.locator(".message.user", { hasText: text })).toHaveCount(
      0,
    );
  });

// A reload right after a message is queued can leave the host without it;
// the page that comes back sends it again under its own id, and the row
// stays queued all along. Its first send may still arrive after that: the
// host takes it once either way.
for (const [how, late] of [
  ["Queue next", false],
  ["Queue next", true],
  ["Enter", false],
])
  test(`a message queued with ${how} that the reload beats to the host stays queued${late ? ", and its late first send is taken once" : ""}`, async ({
    page,
    session,
  }) => {
    const prompt = await start(page, session);
    const text = "Queued as the page reloads.";
    const sent = await queueUnsent(page, prompt, text, how);
    // What every frame after the reload showed of the row.
    await page.addInitScript((text) => {
      const seen = { unconfirmed: false, trayLeft: false };
      globalThis.testSeen = seen;
      let shown = false;
      new MutationObserver(() => {
        const page = document.body?.textContent || "";
        seen.unconfirmed ||= page.includes("Not confirmed");
        const tray = document.querySelector(".guidance-queue");
        const now = !!tray?.textContent?.includes(text);
        seen.trayLeft ||= shown && !now;
        shown ||= now;
      }).observe(document, {
        subtree: true,
        childList: true,
        characterData: true,
      });
    }, text);
    await page.reload();
    await online(page);
    const tray = page.getByRole("region", { name: "Queued messages" });
    await expect(tray).toContainText(text);
    if (late) {
      const answer = await page.request.post("/api/command", {
        data: sent,
        headers: {
          "content-type": "application/json",
          origin: new URL(page.url()).origin,
        },
      });
      expect(answer.ok()).toBe(true);
    }
    expect(await page.evaluate(() => testSeen)).toEqual({
      unconfirmed: false,
      trayLeft: false,
    });
    await tray.getByRole("button", { name: "Insert now" }).click();
    await expect(tray).toHaveCount(0);
    await expect(page.locator(".message.user", { hasText: text })).toHaveCount(
      1,
    );
  });

// With no turn running any more, a send again would start one the person
// did not ask for then: the row stays theirs to send, unconfirmed.
test("a queued message the reload beats to the host, after its turn ended, waits to be sent by hand", async ({
  page,
  session,
  command,
  request,
}) => {
  const prompt = await start(page, session);
  const text = "Queued as the turn ends.";
  await queueUnsent(page, prompt, text, "Queue next");
  const target = { session_id: session.id, generation: session.generation };
  await command("interrupt", target);
  const listed = async () =>
    (await (await request.get("/api/sessions")).json()).sessions.find(
      (item) => item.id === session.id,
    );
  await expect.poll(async () => !!(await listed())?.turn_active).toBe(false);
  await page.reload();
  await online(page);
  await expect(page.locator(".message.user", { hasText: text })).toContainText(
    "Not confirmed",
  );
  await expect(
    page.getByRole("region", { name: "Queued messages" }),
  ).toHaveCount(0);
  expect((await listed()).turn_active).toBeFalsy();
  const { state } = await (
    await request.get(`/api/sessions/${session.id}`)
  ).json();
  expect(state.view.nodes.some((node) => node.body?.text === text)).toBe(false);
});

// The row is shown as queued before the host has said so: taking it back
// then must not reach the host before the message itself has.
test("guidance taken back before its send is answered is taken back once it is", async ({
  page,
  session,
}) => {
  const prompt = await start(page, session);
  let answer;
  await page.route("**/api/command", async (route) => {
    if (route.request().postDataJSON().kind === "steer")
      await new Promise((go) => (answer = go));
    await route.continue();
  });
  await prompt.fill("Held on its way");
  await page
    .getByRole("button", { name: "Send guidance", exact: true })
    .click();
  const tray = page.getByRole("region", { name: "Queued messages" });
  await expect(tray).toContainText("Held on its way");
  await expect.poll(() => typeof answer).toBe("function");
  await tray.getByRole("button", { name: "Edit", exact: true }).click();
  answer();
  await expect(tray).toHaveCount(0);
  await expect(prompt).toHaveValue("Held on its way");
  // The host holds nothing of it: the turn goes on without it.
  await page.unroute("**/api/command");
  await page.reload();
  await expect(page.locator(".message.response")).toContainText(
    "Waiting for guidance",
  );
  await expect(page.locator(".message.user")).toHaveCount(1);
  await expect(
    page.getByRole("region", { name: "Queued messages" }),
  ).toHaveCount(0);
});

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
  await expect(page.locator(".banner")).toContainText(
    "Insertion refused for test",
  );
  await expect(tray).toContainText("Keep this queued");
  await expect(
    tray.getByRole("button", { name: "Edit", exact: true }),
  ).toBeEnabled();
});
