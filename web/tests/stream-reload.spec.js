// Reloading while an answer streams must pick the stream up where the saved
// view left off: the page stays responsive and the answer completes once.
import { test, expect, online } from "./fixtures.js";

test("reload mid-stream resumes the answer without freezing", async ({
  page,
  session,
  command,
}, testInfo) => {
  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
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
  // The main thread keeps painting while the stream catches up.
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
  // Exactly one answer, and its streamed text is not doubled.
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
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  expect(refused).toBeGreaterThan(2);
});
