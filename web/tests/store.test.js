import { test } from "node:test";
import assert from "node:assert/strict";
import {
  liveBlocks,
  api,
  command,
  receiveOutcome,
  applySessionEvent,
  readStored,
} from "../src/store.ts";
import { renderMarkdown, safeURL } from "../src/markdown.ts";

test("parallel tools retain call identity and semantic completion status", () => {
  const events = [
    { type: "response.started" },
    { type: "response.reasoning.delta", data: { text: "supplied" } },
    { type: "tool.call", data: { id: "a", name: "read_file" } },
    { type: "tool.call", data: { id: "b", name: "run" } },
    {
      type: "tool.result",
      data: { id: "b", result: "second", completion_status: "interrupted" },
    },
    {
      type: "tool.result",
      data: {
        id: "a",
        result: "first",
        completion_status: "success",
        duration_ms: 12,
      },
    },
  ];
  const blocks = liveBlocks(events);
  assert.equal(blocks[0].reasoning, "supplied");
  assert.equal(blocks[1].text, "first");
  assert.equal(blocks[1].duration_ms, 12);
  assert.equal(blocks[2].status, "interrupted");
});

test("live answer storage is bounded", () => {
  const blocks = liveBlocks(
    Array.from({ length: 100 }, () => ({
      type: "response.answer.delta",
      data: { text: "x".repeat(2048) },
    })),
  );
  assert.equal(blocks.length, 1);
  assert.equal(blocks[0].text.length, 64 * 1024);
});

test("rendering escapes HTML and never loads remote images", async () => {
  const html = await renderMarkdown(
    "<img src=x onerror=alert(1)>\n\n![private](https://tracker.example/a)\n\n[bad](javascript:alert(1))",
  );
  assert.ok(!html.includes("<img"));
  assert.ok(!html.includes('href="javascript:'));
  assert.ok(html.includes("&lt;img"));
  for (const url of [
    "javascript:x",
    "data:text/html,x",
    "file:///etc/passwd",
    "//evil.example",
  ])
    assert.equal(safeURL(url), false);
  for (const url of [
    "https://example.com",
    "mailto:me@example.com",
    "/relative",
    "#anchor",
  ])
    assert.equal(safeURL(url), true);
});

test("a pending receipt remains pending and SSE may acknowledge before HTTP", async () => {
  const original = globalThis.fetch;
  globalThis.fetch = async () => {
    receiveOutcome({
      request_id: "a".repeat(32),
      accepted: true,
      result: { ok: true },
    });
    return { ok: true, json: async () => ({ accepted: true, pending: true }) };
  };
  try {
    assert.equal((await api("/api/command", {})).pending, true);
    assert.deepEqual(
      (await command("model", null, { request_id: "a".repeat(32) })).result,
      { ok: true },
    );
  } finally {
    globalThis.fetch = original;
  }
});

test("incremental replay retains a long streamed answer and reconciles its stable message", () => {
  let view = { metadata: { incoming: 0 }, state: { view: { blocks: [] } } };
  for (let sequence = 1; sequence <= 1024; sequence++)
    view = applySessionEvent(view, {
      kind: "event",
      type: "response.answer.delta",
      sequence,
      data: { text: "chunk " },
    });
  assert.equal(view.streamed[0].text, "chunk ".repeat(1024));
  const block = { id: "m-3", kind: "assistant", text: "saved", incoming: 1 };
  for (let i = 0; i < 2; i++)
    view = applySessionEvent(view, {
      kind: "event",
      type: "message.changed",
      data: { block },
    });
  assert.equal(view.state.view.blocks.length, 1);
  assert.equal(view.streamed.length, 0);
  assert.equal(view.metadata.incoming, 1);
});

test("corrupt browser preferences fall back without breaking the app", () => {
  for (const value of ["{", "null", '"wrong"', "[]"])
    assert.deepEqual(
      readStored({ getItem: () => value }, "sizes", { display: 100 }),
      { display: 100 },
    );
});

test("saved-history browsing retains only the selected and active views", async () => {
  const { retainedViews } = await import("../src/store.ts");
  const views = Object.fromEntries(
    Array.from({ length: 100 }, (_, index) => [
      String(index),
      { metadata: { status: "saved" } },
    ]),
  );
  views.active = { metadata: { status: "running", generation: "one" } };
  views.crashed = { metadata: { status: "interrupted", generation: "old" } };
  assert.deepEqual(Object.keys(retainedViews(views, "42")).sort(), [
    "42",
    "active",
  ]);
});
