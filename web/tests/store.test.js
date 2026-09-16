import { test } from "node:test";
import assert from "node:assert/strict";
import {
  liveBlocks,
  applySessionEvent,
  readStored,
} from "../src/state/store.ts";
import { api, command, receiveOutcome } from "../src/state/api.ts";
import {
  renderMarkdown,
  renderMarkdownBlocks,
  safeURL,
} from "../src/shared/markdown.ts";

test("parallel tools retain call identity and semantic completion status", () => {
  const events = [
    { type: "response.started", data: { response_id: "r-1" } },
    {
      type: "response.reasoning.delta",
      data: { response_id: "r-1", text: "supplied" },
    },
    {
      type: "tool.call",
      data: {
        response_id: "r-1",
        occurrence_id: "r-1:a",
        call_id: "a",
        name: "read_file",
      },
    },
    {
      type: "tool.call",
      data: {
        response_id: "r-1",
        occurrence_id: "r-1:b",
        call_id: "b",
        name: "run",
      },
    },
    {
      type: "tool.result",
      data: {
        response_id: "r-1",
        occurrence_id: "r-1:b",
        call_id: "b",
        result: "second",
        completion_status: "interrupted",
      },
    },
    {
      type: "tool.result",
      data: {
        response_id: "r-1",
        occurrence_id: "r-1:a",
        call_id: "a",
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

test("async receipts retain durable identities and never fabricate tool calls", () => {
  const notification = {
    type: "activity.completed",
    data: { id: 7, status: 0 },
  };
  assert.deepEqual(liveBlocks([notification]), []);
  let snapshot = { state: { view: { blocks: [] } } };
  for (const id of ["m-12", "m-13", "m-12"]) {
    snapshot = applySessionEvent(snapshot, {
      kind: "event",
      type: "message.changed",
      data: {
        block: {
          id,
          kind: "activity",
          activity_id: 7,
          text: "Completed",
          status: "completed",
        },
      },
    });
  }
  assert.deepEqual(
    snapshot.state.view.blocks.map((b) => b.id),
    ["m-12", "m-13"],
  );
  assert.ok(
    snapshot.state.view.blocks.every(
      (b) => b.kind === "activity" && !b.call_id,
    ),
  );
});

test("live answer storage is bounded", () => {
  const blocks = liveBlocks([
    { type: "response.started", data: { response_id: "r-long" } },
    ...Array.from({ length: 100 }, () => ({
      type: "response.answer.delta",
      data: { response_id: "r-long", text: "x".repeat(2048) },
    })),
  ]);
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

test("only Markdown code blocks receive copy controls", async () => {
  const source =
    "Prose with `inline code`.\n\n    indented <code> & spaces\n\n```text\nfenced <code> & spaces\n```";
  const blocks = await renderMarkdownBlocks(source);
  const html = blocks.map((block) => block.html).join("");
  assert.equal(blocks.filter((block) => block.code).length, 2);
  assert.equal((html.match(/class="code-copy"/g) || []).length, 0);
  assert.equal(html, await renderMarkdown(source));
  assert.ok(html.includes("<p>Prose with <code>inline code</code>.</p>"));
  assert.ok(html.includes("indented &lt;code&gt; &amp; spaces\n</code>"));
  assert.ok(html.includes("fenced &lt;code&gt; &amp; spaces\n</code>"));
});

test("a pending receipt remains pending and SSE may acknowledge before HTTP", async () => {
  const original = globalThis.fetch;
  globalThis.fetch = async () => {
    receiveOutcome({
      request_id: "a".repeat(32),
      accepted: true,
      pending: true,
    });
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
  view = applySessionEvent(view, {
    kind: "event",
    type: "response.started",
    sequence: 0,
    data: { response_id: "r-3" },
  });
  for (let sequence = 1; sequence <= 1024; sequence++)
    view = applySessionEvent(view, {
      kind: "event",
      type: "response.answer.delta",
      sequence,
      data: { response_id: "r-3", text: "chunk " },
    });
  assert.equal(view.streamed[0].text, "chunk ".repeat(1024));
  const block = {
    id: "m-3",
    response_id: "r-3",
    kind: "assistant",
    text: "saved",
    incoming: 1,
  };
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

test("tool results keep their assistant response and live sibling state", () => {
  let view = {
    metadata: {},
    state: {
      view: {
        blocks: [
          {
            id: "m-1",
            kind: "assistant",
            response_id: "r-1",
            reasoning: "retained thinking",
          },
        ],
      },
    },
  };
  for (const occurrence of ["r-1:a", "r-1:b"])
    view = applySessionEvent(view, {
      kind: "event",
      type: "tool.call",
      data: {
        response_id: "r-1",
        occurrence_id: occurrence,
        call_id: occurrence.at(-1),
      },
    });
  const result = (id, occurrence, text) => ({
    kind: "event",
    type: "message.changed",
    data: {
      block: {
        id,
        kind: "tool_result",
        response_id: "r-1",
        occurrence_id: occurrence,
        status: "success",
        text,
      },
    },
  });
  view = applySessionEvent(view, result("m-2", "r-1:a", "first"));
  assert.equal(view.state.view.blocks[0].kind, "assistant");
  assert.equal(view.state.view.blocks[0].reasoning, "retained thinking");
  assert.equal(view.state.view.blocks[1].occurrence_id, "r-1:a");
  assert.deepEqual(
    view.streamed.map((block) => block.occurrence_id),
    ["r-1:b"],
  );
  view = applySessionEvent(view, result("m-3", "r-1:b", "second"));
  assert.deepEqual(
    view.state.view.blocks.map((block) => block.kind),
    ["assistant", "tool_result", "tool_result"],
  );
  assert.equal(view.streamed.length, 0);
});

test("final previews and checkpoints cannot downgrade a fuller response revision", () => {
  const full = "complete streamed body ".repeat(400);
  let view = {
    metadata: { incoming: 0 },
    state: { view: { blocks: [] } },
  };
  view = applySessionEvent(view, {
    kind: "event",
    type: "response.started",
    data: { response_id: "r-final" },
  });
  view = applySessionEvent(view, {
    kind: "event",
    type: "response.answer.delta",
    data: { response_id: "r-final", text: full },
  });
  view = applySessionEvent(view, {
    kind: "event",
    type: "message.changed",
    data: {
      block: {
        id: "m-final",
        response_id: "r-final",
        kind: "assistant",
        text: full.slice(0, 4096),
        text_bytes: 4096,
        content_revision: 1,
        content_complete: false,
        truncated: true,
      },
    },
  });
  assert.equal(view.state.view.blocks[0].text, full);
  view = applySessionEvent(view, {
    kind: "event",
    type: "message.changed",
    data: {
      block: {
        ...view.state.view.blocks[0],
        text: full.slice(0, 4096),
        text_bytes: 4096,
        truncated: true,
      },
    },
  });
  assert.equal(view.state.view.blocks[0].text, full);
});

test("corrupt browser preferences fall back without breaking the app", () => {
  for (const value of ["{", "null", '"wrong"', "[]"])
    assert.deepEqual(
      readStored({ getItem: () => value }, "sizes", { display: 100 }),
      { display: 100 },
    );
});

test("saved-history browsing retains the selected view and four recent views", async () => {
  const { retainedViews } = await import("../src/state/store.ts");
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
    "98",
    "99",
    "active",
    "crashed",
  ]);
});

test("live context replaces the estimate without accumulating billing tokens", () => {
  const blocks = [{ id: "stable", kind: "assistant", text: "unchanged" }];
  const view = { blocks };
  const current = {
    state: { context_tokens: 1000, usage: { cost: 0.01 }, view },
  };
  const growing = applySessionEvent(current, {
    kind: "event",
    type: "usage.updated",
    data: { context_tokens: 2400, usage: { cost: 0.01 } },
  });
  assert.equal(growing.state.context_tokens, 2400);
  assert.equal(growing.state.usage.cost, 0.01);
  assert.equal(growing.state.view, view);
  assert.equal(growing.state.view.blocks, blocks);
  const compacted = applySessionEvent(growing, {
    kind: "event",
    type: "usage.updated",
    data: { context_tokens: 0 },
  });
  assert.equal(compacted.state.context_tokens, 0);
  assert.equal(current.state.context_tokens, 1000);
});

test("collaborator lifecycle events upsert and remove one retained runtime", () => {
  const current = {
    state: { collaborators: [{ id: "ordinary", status: "idle" }] },
  };
  const running = applySessionEvent(current, {
    kind: "event",
    type: "collaborator.changed",
    data: {
      collaborator: {
        id: "sidekick",
        label: "Review implementation",
        status: "running",
        persistent: true,
      },
    },
  });
  assert.deepEqual(
    running.state.collaborators.map((item) => item.id),
    ["ordinary", "sidekick"],
  );
  const idle = applySessionEvent(running, {
    kind: "event",
    type: "collaborator.changed",
    data: { collaborator: { id: "sidekick", status: "idle" } },
  });
  assert.equal(idle.state.collaborators[1].status, "idle");
  const removed = applySessionEvent(idle, {
    kind: "event",
    type: "collaborator.changed",
    data: { collaborator: { id: "sidekick" }, removed: true },
  });
  assert.deepEqual(removed.state.collaborators, [
    { id: "ordinary", status: "idle" },
  ]);
});
