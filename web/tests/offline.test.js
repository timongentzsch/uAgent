import { test } from "node:test";
import assert from "node:assert/strict";

Object.defineProperty(globalThis, "localStorage", {
  configurable: true,
  value: { getItem: () => null },
});
const { cachedSnapshot, mergeCached } = await import("../src/offline.ts");
const snapshot = (blocks, options = {}) => ({
  cursor: 1,
  epoch: "host",
  metadata: { id: "a", turn_active: true, pending: true, presence: "terminal" },
  state: {
    http: [{ id: "wire" }],
    activities: [{}],
    collaborators: [{}],
    view: { blocks, more: false, dropped_segments: 0, ...options },
  },
});

test("offline projection removes transport and live state without changing saved facts", () => {
  const original = snapshot([
    {
      id: "m-1",
      text: "saved",
      http: [{ id: "wire" }],
      summary: { tool_calls: 3 },
    },
  ]);
  const value = cachedSnapshot(original);
  assert.equal(value.metadata.turn_active, false);
  assert.equal(value.metadata.pending, false);
  assert.equal(value.metadata.presence, "");
  assert.equal(value.state.http, undefined);
  assert.equal(value.state.view.blocks[0].http, undefined);
  assert.equal(value.state.view.blocks[0].summary.tool_calls, 3);
  assert.equal(original.metadata.turn_active, true);
  assert.equal(original.state.view.blocks[0].http.length, 1);
});

test("cached pagination preserves expanded content but rejects another history epoch", () => {
  const old = snapshot([
    { id: "m-1", sequence: 1, text: "first" },
    { id: "m-2", sequence: 2, text: "full retained output", truncated: false },
  ]);
  const latest = snapshot(
    [{ id: "m-2", sequence: 2, text: "full", truncated: true }],
    { before: 2, more: true },
  );
  const merged = mergeCached(old, latest);
  assert.equal(merged.state.view.blocks.length, 2);
  assert.equal(merged.state.view.blocks[1].text, "full retained output");
  assert.equal(merged.state.view.more, false);
  const restarted = { ...latest, epoch: "new host" };
  assert.equal(mergeCached(old, restarted), restarted);
  const compacted = snapshot(latest.state.view.blocks, { dropped_segments: 1 });
  assert.equal(mergeCached(old, compacted), compacted);
});
