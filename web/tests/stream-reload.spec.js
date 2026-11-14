// Reloading while an answer streams must pick the stream up where the saved
// view left off: the page stays responsive and the answer completes once.
import {
  connected,
  test,
  expect,
  online,
  selectProbeModel,
} from "./fixtures.js";

test("reload mid-stream resumes the answer without freezing", async ({
  page,
  session,
  command,
}, testInfo) => {
  await selectProbeModel(command, session);
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Long continuity probe");
  await prompt.press("Enter");
  const answer = page.locator(".message.response > .markdown");
  await expect
    .poll(async () => (await answer.last().textContent())?.length || 0)
    .toBeGreaterThan(1500);

  await page.reload();
  const gaps = page.evaluate(
    () =>
      new Promise((resolve) => {
        const gaps = [];
        let previous = performance.now();
        const tick = (now) => {
          gaps.push(now - previous);
          previous = now;
          if (gaps.length < 90) requestAnimationFrame(tick);
          else resolve(gaps);
        };
        requestAnimationFrame(tick);
      }),
  );
  await expect(answer.last()).toContainText(
    "unfinished-looking content retained safely",
    { timeout: 20_000 },
  );
  await expect(page.locator(".composer .status-led.running")).toHaveCount(0);
  await expect(answer).toHaveCount(1);
  const text = (await answer.textContent()) || "";
  expect(text.split("Stable opening paragraph").length - 1).toBe(1);
  expect(text.split("Long retained body sentence.").length - 1).toBe(360);
  // Ninety frames were painted meanwhile; how far apart is a measurement.
  await testInfo.attach("reload-frame-gaps.json", {
    body: JSON.stringify(await gaps),
    contentType: "application/json",
  });
});

test("an event stream refused mid-restart reconnects on its own", async ({
  page,
  session,
}) => {
  // A proxy's 502 closes an EventSource for good; the app must retry.
  let refused = 0;
  await page.route("**/api/events*", (route) =>
    refused++ < 2 ? route.fulfill({ status: 502, body: "" }) : route.continue(),
  );
  await page.goto(`/#session=${session.id}`);
  await expect(connected(page)).toBeVisible();
  expect(refused).toBeGreaterThan(2);
});

test("a reconnect reopens the event stream at its cursor and reads nothing again", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session);
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const requests = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.pathname.startsWith("/api/"))
      requests.push(url.pathname + url.search);
  });
  await page.evaluate(() => {
    dispatchEvent(new Event("offline"));
    dispatchEvent(new Event("online"));
  });
  await expect
    .poll(() => requests.some((path) => path.startsWith("/api/events?cursor=")))
    .toBe(true);
  await online(page);
  expect(requests.filter((path) => path.startsWith("/api/sessions"))).toEqual(
    [],
  );
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Hello there");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.locator(".message.response").first()).toBeVisible();
});

// A reload leaves the composer on screen before the event stream has caught
// up. `hold` keeps the stream waiting until it is released.
async function holdStream(page) {
  let release;
  const held = new Promise((resolve) => {
    release = resolve;
  });
  await page.route("**/api/events*", async (route) => {
    await held;
    await route.continue();
  });
  return release;
}

// The commands sent so far whose body carries `text`.
function commandsWith(page, text) {
  const sent = [];
  page.on("request", (request) => {
    if (
      request.method() === "POST" &&
      new URL(request.url()).pathname === "/api/command" &&
      request.postData()?.includes(text)
    )
      sent.push(request.postData());
  });
  return sent;
}

async function openOnline(page, session, command) {
  await selectProbeModel(command, session);
  await page.goto(`/#session=${session.id}`);
  await online(page);
}

test("a message sent while reconnecting waits and goes out once", async ({
  page,
  session,
  command,
}) => {
  await openOnline(page, session, command);
  const sent = commandsWith(page, "Waiting probe");
  const release = await holdStream(page);
  await page.reload();
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Waiting probe");
  await prompt.press("Enter");
  const row = page.locator(".message.user", { hasText: "Waiting probe" });
  await expect(row).toContainText("Waiting for connection");
  await expect(prompt).toHaveValue("");
  expect(sent).toHaveLength(0);
  release();
  await expect(page.locator(".message.response").first()).toBeVisible();
  await expect(row).toHaveCount(1);
  expect(sent).toHaveLength(1);
});

test("a reload while a message waits neither loses nor repeats it", async ({
  page,
  session,
  command,
}) => {
  await openOnline(page, session, command);
  const sent = commandsWith(page, "Waiting probe");
  const release = await holdStream(page);
  await page.reload();
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Waiting probe");
  await prompt.press("Enter");
  const row = page.locator(".message.user", { hasText: "Waiting probe" });
  await expect(row).toContainText("Waiting for connection");
  await page.reload();
  await expect(row).toContainText("Waiting for connection");
  expect(sent).toHaveLength(0);
  release();
  await expect(page.locator(".message.response").first()).toBeVisible();
  await expect(row).toHaveCount(1);
  expect(sent).toHaveLength(1);
});

test("a message waiting for the connection returns to the composer", async ({
  page,
  session,
  command,
}) => {
  await openOnline(page, session, command);
  const sent = commandsWith(page, "Waiting probe");
  const release = await holdStream(page);
  await page.reload();
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Waiting probe");
  await prompt.press("Enter");
  await page
    .getByRole("button", { name: "Return message to composer" })
    .click();
  await expect(prompt).toHaveValue("Waiting probe");
  await expect(page.locator(".message.user")).toHaveCount(0);
  release();
  await online(page);
  expect(sent).toHaveLength(0);
  await expect(prompt).toHaveValue("Waiting probe");
});

test("a message waiting while a turn runs goes out as guidance", async ({
  page,
  session,
  command,
}) => {
  await openOnline(page, session, command);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Queue controls probe");
  await prompt.press("Enter");
  await expect(page.locator(".message.response")).toContainText(
    "Waiting for guidance",
  );
  const sent = commandsWith(page, "Steer probe");
  const release = await holdStream(page);
  await page.reload();
  await prompt.fill("Steer probe");
  await prompt.press("Enter");
  await expect(
    page.locator(".message.user", { hasText: "Steer probe" }),
  ).toContainText("Waiting for connection");
  release();
  await expect(
    page.getByRole("region", { name: "Queued messages" }),
  ).toContainText("Steer probe");
  expect(sent).toHaveLength(1);
  expect(sent[0]).toContain('"steer"');
});
