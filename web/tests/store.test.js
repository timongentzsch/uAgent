import { test } from "node:test";
import assert from "node:assert/strict";
import {
  applySessionEvent,
  confirmOutgoing,
  keepOlderPages,
  patchCatalogue,
  raiseIncoming,
  readStored,
  stateFrame,
  withActivities,
} from "../src/state/store.ts";
import { api, command, receiveOutcome } from "../src/state/api.ts";
import { maxHttpExchanges } from "../src/shared/limits.ts";
import {
  renderMarkdown,
  renderMarkdownBlocks,
  safeURL,
} from "../src/shared/markdown.ts";

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

test("HTTP exchanges update in place by id and stay bounded", () => {
  const exchange = (id, state) => ({
    kind: "event",
    type: "http.exchange",
    data: { id, state },
  });
  let current = applySessionEvent({}, exchange("a", "pending"));
  current = applySessionEvent(current, exchange("b", "pending"));
  current = applySessionEvent(current, exchange("a", "done"));
  assert.deepEqual(
    current.state.http.map((item) => [item.id, item.state]),
    [
      ["a", "done"],
      ["b", "pending"],
    ],
  );
  for (let index = 0; index <= maxHttpExchanges; index++)
    current = applySessionEvent(current, exchange(`n${index}`, "done"));
  assert.equal(current.state.http.length, maxHttpExchanges);
  assert.equal(current.state.http.at(-1).id, `n${maxHttpExchanges}`);
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
    kind: "block",
    sequence: 401,
    block: { id: "m-400", sequence: 400, kind: "assistant", text: "new" },
  });
  assert.equal(next.state.view.blocks.length, 401);
  assert.equal(next.state.view.blocks[0], blocks[0]);
  assert.equal(next.state.view.blocks.at(-1).text, "new");
  // A checkpoint replaces the host's window and keeps the loaded pages.
  const checkpoint = keepOlderPages(next, {
    ...next,
    state: {
      view: {
        blocks: [{ id: "m-400", sequence: 400, kind: "assistant", text: "x" }],
        before: 400,
      },
    },
  });
  assert.equal(checkpoint.state.view.blocks.length, 401);
  assert.equal(checkpoint.state.view.blocks.at(-1).text, "x");
  // An in-place rewind starts a new view epoch: older pages go with it.
  const rewound = keepOlderPages(next, {
    ...next,
    state: {
      view_epoch: 1,
      view: {
        blocks: [{ id: "m-2", sequence: 2, kind: "user", text: "again" }],
        before: 2,
      },
    },
  });
  assert.equal(rewound.state.view.blocks.length, 1);
});

// The browser applies the host's rows and never re-derives them: a whole row,
// streamed appends, set fields, and a late retained row in sequence position.
test("block patches rebuild the host's view", () => {
  const patch = (value) => ({ kind: "block", ...value });
  let snapshot = { cursor: 0, metadata: {}, state: { view: { blocks: [] } } };
  for (const value of [
    patch({ block: { id: "r", kind: "assistant", text: "", streaming: true } }),
    patch({ id: "r", append: { text: "Hello" } }),
    patch({ id: "r", append: { text: " world", reasoning: "why" } }),
    patch({ id: "r", set: { streaming: false } }),
    patch({ block: { id: "t-a", kind: "tool_result", status: "running" } }),
    patch({ block: { id: "t-a", kind: "tool_result", status: "success" } }),
    patch({ block: { id: "m-1", sequence: 1, kind: "user", text: "hi" } }),
    patch({ id: "missing", append: { text: "dropped" } }),
  ])
    snapshot = applySessionEvent(snapshot, value);
  const blocks = snapshot.state.view.blocks;
  assert.deepEqual(
    blocks.map((block) => block.id),
    ["r", "t-a", "m-1"],
  );
  assert.equal(blocks[0].text, "Hello world");
  assert.equal(blocks[0].reasoning, "why");
  assert.equal(blocks[0].streaming, false);
  assert.equal(blocks[1].status, "success");
  const retained = applySessionEvent(snapshot, {
    kind: "block",
    block: { id: "m-0", sequence: 0, kind: "user", text: "first" },
  });
  assert.equal(retained.state.view.blocks.at(-2).id, "m-0");
});

// The shell re-renders on identity, so an update that changes nothing must
// hand back the very object it was given.
test("a catalogue update that changes nothing keeps the catalogue's identity", () => {
  const a = { id: "a", incoming: 2, activities: [{ id: "1" }] };
  const b = { id: "b" };
  const catalogue = { sessions: [a, b], devices: [], capabilities: {} };
  const raise = (to) =>
    patchCatalogue(catalogue, "a", (item) => raiseIncoming(item, to));
  assert.equal(raise(2), catalogue);
  assert.equal(raise(1), catalogue);
  const raised = raise(3);
  assert.equal(raised.sessions[0].incoming, 3);
  assert.equal(raised.sessions[1], b);
  assert.equal(raised.devices, catalogue.devices);
  const activities = (value) =>
    patchCatalogue(catalogue, "a", (item) => withActivities(item, value));
  assert.equal(activities([{ id: "1" }]), catalogue);
  assert.deepEqual(activities([]).sessions[0].activities, []);
  assert.equal(
    patchCatalogue(catalogue, "missing", () => ({ id: "missing" })),
    catalogue,
  );
});

test("the host's row removes the outgoing one for its request, and only that", () => {
  const outgoing = [{ request_id: "r1" }, { request_id: "r2" }];
  assert.deepEqual(confirmOutgoing(outgoing, "r1"), [{ request_id: "r2" }]);
  assert.equal(confirmOutgoing(outgoing, "r3"), outgoing);
  // A row with no request (history, another client's message) confirms none.
  assert.equal(confirmOutgoing(outgoing, undefined), outgoing);
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

test("activity status retains its provenance", () => {
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

test("a state frame without the view keeps the last view, HTTP log and prompt", () => {
  const view = { blocks: [{ id: "a", kind: "user", text: "hi" }] };
  const http = [{ id: "h", state: "done" }];
  const prior = { phase: "idle", view, http, system_prompt: "SYS", turns: 1 };
  assert.deepEqual(stateFrame(prior, { turns: 2 }, "model"), {
    view,
    http,
    system_prompt: "SYS",
    turns: 2,
    phase: "model",
  });
  // A checkpoint frame still replaces all three.
  const next = { blocks: [] };
  const checkpoint = stateFrame(
    prior,
    { view: next, http: [], system_prompt: "NEW" },
    "idle",
  );
  assert.equal(checkpoint.view, next);
  assert.deepEqual(checkpoint.http, []);
  assert.equal(checkpoint.system_prompt, "NEW");
});
