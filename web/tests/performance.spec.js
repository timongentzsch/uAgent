import { test, expect } from "./fixtures.js";
import { writeFile } from "node:fs/promises";

// Deterministic transport workload: each supplied word represents one mock
// token. This measures browser processing, not provider tokenization/network.
test("streaming workload preserves content and remains interactive", async ({
  page,
  session,
  request,
}, testInfo) => {
  await page.addInitScript(() => {
    const NativeEventSource = EventSource;
    globalThis.testStreams = [];
    globalThis.EventSource = class extends NativeEventSource {
      constructor(...args) {
        super(...args);
        globalThis.testStreams.push(this);
      }
    };
  });
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  const started = performance.now();
  await page.goto(`/#session=${session.id}`);
  await expect(page.getByLabel("Message or guidance")).toBeEnabled();
  await expect(page.locator(".composer").getByRole("status")).toHaveText(
    "Ready",
  );
  const startupMs = performance.now() - started;
  const snapshot = await (
    await request.get(`/api/sessions/${session.id}`)
  ).json();
  const streaming = page.evaluate(
    async ({ session, snapshot }) => {
      const stream = testStreams.at(-1);
      let sequence = snapshot.cursor + 1;
      const response = "throughput-probe";
      const send = (type, data) =>
        stream.dispatchEvent(
          new MessageEvent("update", {
            data: JSON.stringify({
              v: 2,
              epoch: snapshot.epoch,
              sequence: sequence++,
              session_id: session.id,
              generation: session.generation,
              kind: "event",
              type,
              data,
            }),
          }),
        );
      const gaps = [];
      let previous = performance.now(),
        frame;
      const tick = (now) => {
        gaps.push(now - previous);
        previous = now;
        frame = requestAnimationFrame(tick);
      };
      frame = requestAnimationFrame(tick);
      send("response.started", { response_id: response });
      const start = performance.now();
      let text = "",
        sent = 0;
      // 5,000 mock tokens over five seconds, paced by elapsed time so timer
      // scheduling does not silently reduce the offered workload under load.
      await new Promise((resolve) => {
        const timer = setInterval(() => {
          const due = Math.min(5000, Math.floor(performance.now() - start));
          const chunk = "word ".repeat(due - sent);
          if (chunk) {
            text += chunk;
            send("response.answer.delta", {
              response_id: response,
              text: chunk,
            });
          }
          sent = due;
          if (sent === 5000) {
            clearInterval(timer);
            resolve();
          }
        }, 20);
      });
      send("response.finished", { response_id: response });
      cancelAnimationFrame(frame);
      gaps.sort((a, b) => a - b);
      return {
        mockTokens: sent,
        text,
        elapsedMs: performance.now() - start,
        frameGapP95Ms: gaps[Math.floor(gaps.length * 0.95)],
        longestFrameGapMs: gaps.at(-1),
        resources: performance.getEntriesByType("resource").length,
        transferredBytes: performance
          .getEntriesByType("resource")
          .reduce((sum, entry) => sum + entry.transferSize, 0),
      };
    },
    { session, snapshot },
  );
  const response = page.locator('[data-message-id="throughput-probe"]');
  await expect(response).toContainText("word");
  const duringStart = performance.now();
  await page.getByLabel("Message or guidance").fill("Typing while streaming");
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "Typing while streaming",
  );
  const inputDuringStreamMs = performance.now() - duringStart;
  const metrics = await streaming;
  await expect(response).toContainText(metrics.text.trim());
  const inputStart = performance.now();
  await page.getByLabel("Message or guidance").fill("Still responsive");
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "Still responsive",
  );
  delete metrics.text;
  const result = {
    project: testInfo.project.name,
    startupMs,
    inputDuringStreamMs,
    inputRoundTripMs: performance.now() - inputStart,
    ...metrics,
  };
  await testInfo.attach("web-performance.json", {
    body: JSON.stringify(result, null, 2),
    contentType: "application/json",
  });
  if (process.env.UAGENT_PROFILE_OUTPUT)
    await writeFile(
      process.env.UAGENT_PROFILE_OUTPUT,
      JSON.stringify(result, null, 2),
    );
  expect(errors).toEqual([]);
});
