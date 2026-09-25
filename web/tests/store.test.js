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
  // A blocked image keeps its alt text as a readable placeholder.
  assert.ok(html.includes("[image: private]"));
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

test("identity-less retained tool rows rejoin by call keys in sequence order", () => {
  // A live transient from an older turn plus a newer retained row already
  // in view. The retained completion arrives WITHOUT occurrence/response
  // identity (facts recorded after the emit, legacy paths): it must still
  // clear the transient and land in sequence position — never appended
  // after newer rows (the 6:15-after-6:24 report).
  let view = {
    metadata: {},
    state: {
      view: {
        blocks: [
          { id: "m-1", sequence: 1, kind: "assistant", response_id: "r-old" },
          { id: "m-9", sequence: 9, kind: "assistant", response_id: "r-new" },
        ],
      },
    },
  };
  for (const type of ["tool.call", "tool.result"])
    view = applySessionEvent(view, {
      kind: "event",
      type,
      data: {
        response_id: "r-old",
        occurrence_id: "r-old:a",
        call_id: "a",
        detail_id: "t-old-a",
        ...(type === "tool.result"
          ? { result: "old output", completion_status: "success" }
          : { name: "run" }),
      },
    });
  assert.equal(view.streamed.length, 1);
  view = applySessionEvent(view, {
    kind: "event",
    type: "message.changed",
    data: {
      block: {
        id: "m-5",
        sequence: 5,
        kind: "tool_result",
        call_id: "a",
        detail_id: "t-old-a",
        name: "run",
        status: "complete",
        text: "old output",
      },
    },
  });
  assert.equal(view.streamed.length, 0);
  assert.deepEqual(
    view.state.view.blocks.map((block) => block.id),
    ["m-1", "m-5", "m-9"],
  );
});

test("repeated provider call ids across turns never cross-merge", () => {
  // Same bare call id streaming in a new turn under a fresh detail id:
  // a retained completion for the older turn (same call id, old detail)
  // must leave the live transient alone.
  let view = { metadata: {}, state: { view: { blocks: [] } } };
  view = applySessionEvent(view, {
    kind: "event",
    type: "tool.call",
    data: {
      response_id: "r-new",
      occurrence_id: "r-new:a",
      call_id: "a",
      detail_id: "t-new-a",
      name: "run",
    },
  });
  view = applySessionEvent(view, {
    kind: "event",
    type: "message.changed",
    data: {
      block: {
        id: "m-1",
        sequence: 1,
        kind: "tool_result",
        call_id: "a",
        detail_id: "t-old-a",
        status: "complete",
        text: "older turn output",
      },
    },
  });
  assert.deepEqual(
    view.streamed.map((block) => block.occurrence_id),
    ["r-new:a"],
  );
  assert.deepEqual(
    view.state.view.blocks.map((block) => block.id),
    ["m-1"],
  );
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

test("a retained arrival preserves older pages explicitly loaded by the user", () => {
  const blocks = Array.from({ length: 400 }, (_, sequence) => ({
    id: `m-${sequence}`,
    sequence,
    kind: "user",
    text: "retained",
  }));
  const current = {
    metadata: { incoming: 0 },
    state: { view: { blocks } },
    cursor: 400,
  };
  const next = applySessionEvent(current, {
    kind: "event",
    type: "message.changed",
    sequence: 401,
    data: {
      block: { id: "m-400", sequence: 400, kind: "assistant", text: "new" },
    },
  });
  assert.equal(next.state.view.blocks.length, 401);
  assert.equal(next.state.view.blocks[0], blocks[0]);
  assert.equal(next.state.view.blocks.at(-1).text, "new");
});

test("closing a view aborts its pending command wait", async () => {
  const original = globalThis.fetch;
  globalThis.fetch = async () => ({
    ok: true,
    json: async () => ({ accepted: true, pending: true }),
  });
  try {
    const controller = new AbortController();
    const pending = command(
      "activity",
      null,
      { operation: "inspect" },
      { signal: controller.signal },
    );
    await Promise.resolve();
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
  } finally {
    globalThis.fetch = original;
  }
});

test("reasoning final corrections replace the preview and activity retains provenance", () => {
  const events = [
    { type: "response.started", data: { response_id: "r" } },
    {
      type: "response.reasoning.delta",
      data: { response_id: "r", text: "Draft" },
    },
    {
      type: "response.reasoning.delta",
      data: { response_id: "r", text: "Final", reset: true },
    },
    {
      type: "response.reasoning.delta",
      data: { response_id: "r", text: " summary" },
    },
  ];
  assert.equal(liveBlocks(events)[0].reasoning, "Final summary");
  const detail = {
    source: "model_intent",
    label: "Running · Checking tests",
    active_tools: 2,
  };
  const next = applySessionEvent(
    { cursor: 0, metadata: { id: "s" } },
    {
      kind: "activity",
      phase: "tool",
      activity: detail.label,
      activity_detail: detail,
    },
  );
  assert.equal(next.state.phase, "tool");
  assert.deepEqual(next.state.activity_detail, detail);
});
