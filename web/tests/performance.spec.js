import { test, expect } from "./fixtures.js";
import { writeFile } from "node:fs/promises";

// Deterministic transport workload: each supplied word represents one mock
// token. This measures browser processing, not provider tokenization/network.
for (const labels of [false, true])
  test(`streaming workload remains interactive (captions ${labels ? "on" : "off"})`, async ({
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
    const started = performance.now();
    await page.goto(`/#session=${session.id}`);
    await expect(page.getByLabel("Message or guidance")).toBeEnabled();
    await expect(page.locator(".composer .activity-caption")).toHaveText(
      "Ready",
    );
    const startupMs = performance.now() - started;
    const snapshot = await (
      await request.get(`/api/sessions/${session.id}`)
    ).json();
    const streaming = page.evaluate(
      async ({ session, snapshot, labels }) => {
        const stream = testStreams.at(-1);
        let sequence = snapshot.cursor + 1;
        const response = "throughput-probe";
        let emitted = 0;
        const send = (type, data, kind = "event") => {
          ++emitted;
          stream.dispatchEvent(
            new MessageEvent("update", {
              data: JSON.stringify({
                v: 2,
                epoch: snapshot.epoch,
                sequence: sequence++,
                session_id: session.id,
                generation: session.generation,
                kind,
                ...(kind === "activity" || kind === "block" ? data : {}),
                type,
                data,
              }),
            }),
          );
        };
        // Rows arrive the way the host sends them: whole rows and appends.
        const patch = (value) => send("", value, "block");
        const gaps = [];
        let previous = performance.now(),
          frame;
        const tick = (now) => {
          gaps.push(now - previous);
          previous = now;
          frame = requestAnimationFrame(tick);
        };
        frame = requestAnimationFrame(tick);
        patch({
          block: {
            id: response,
            response_id: response,
            kind: "assistant",
            text: "",
            reasoning: "",
            streaming: true,
          },
        });
        const start = performance.now();
        let text = "",
          sent = 0,
          action = -1;
        // 5,000 mock tokens over five seconds, paced by elapsed time so timer
        // scheduling does not silently reduce the offered workload under load.
        await new Promise((resolve) => {
          const timer = setInterval(() => {
            const due = Math.min(5000, Math.floor(performance.now() - start));
            const chunk = "word ".repeat(due - sent);
            if (chunk) {
              text += chunk;
              patch({
                id: response,
                append: { text: chunk, reasoning: chunk },
              });
            }
            const nextAction = Math.floor(due / 1000);
            if (nextAction !== action) {
              action = nextAction;
              send("activities.changed", {
                activities: ["child-a", "child-b"].map((id, index) => ({
                  id: index + 1,
                  kind: "agent",
                  agent_id: id,
                  name: id,
                  status: "running",
                  progress: `Thinking · Reviewing module ${action}`,
                })),
              });
              const row = (id, status, text) => ({
                block: {
                  id: `t-${action}-${id}`,
                  kind: "tool_result",
                  response_id: response,
                  name: "read_path",
                  arguments: JSON.stringify({ path: `module-${action}.ts` }),
                  status,
                  text,
                },
              });
              for (const id of ["a", "b"]) patch(row(id, "running", ""));
              if (labels)
                send(
                  "",
                  {
                    phase: "tool",
                    activity: `Running · Reviewing module ${action} · +1 tools`,
                    activity_detail: { source: "tool", active_tools: 2 },
                  },
                  "activity",
                );
              for (const id of ["a", "b"])
                patch(row(id, "success", "inspected"));
            }
            sent = due;
            if (sent === 5000) {
              clearInterval(timer);
              resolve();
            }
          }, 20);
        });
        patch({ id: response, set: { streaming: false } });
        cancelAnimationFrame(frame);
        gaps.sort((a, b) => a - b);
        return {
          captions: labels,
          emittedEvents: emitted,
          mockTokens: sent,
          reasoningMockTokens: sent,
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
      { session, snapshot, labels },
    );
    const response = page.locator(
      '.message.response[data-message-id="throughput-probe"]',
    );
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
    // Streaming 5,000 tokens with tool rows keeps painting every frame and
    // answering input; the bounds leave room for slow CI machines.
    expect(result.frameGapP95Ms).toBeLessThan(50);
    expect(result.inputDuringStreamMs).toBeLessThan(1000);
    await testInfo.attach("web-performance.json", {
      body: JSON.stringify(result, null, 2),
      contentType: "application/json",
    });
    if (process.env.UAGENT_PROFILE_OUTPUT)
      await writeFile(
        `${process.env.UAGENT_PROFILE_OUTPUT}.${labels ? "captions" : "baseline"}.json`,
        JSON.stringify(result, null, 2),
      );
  });
