import test from "node:test";
import assert from "node:assert/strict";
import { NONE, kept, ranged, toggled } from "../src/shared/selection.ts";

const order = ["/ws", "a", "b", "c", "/folder", "d"];
const ids = (state) => [...state.ids].sort();

test("⌘-click adds and removes one row and moves the anchor", () => {
  const one = toggled(NONE, "b");
  assert.deepEqual(ids(one), ["b"]);
  assert.equal(one.anchor, "b");
  assert.deepEqual(ids(toggled(one, "b")), []);
});

test("Shift-click adds the run from the anchor, either way, across places", () => {
  const from = toggled(NONE, "a");
  assert.deepEqual(ids(ranged(from, "d", order)), [
    "/folder",
    "a",
    "b",
    "c",
    "d",
  ]);
  assert.deepEqual(ids(ranged(toggled(NONE, "c"), "/ws", order)), [
    "/ws",
    "a",
    "b",
    "c",
  ]);
  const more = ranged(toggled(toggled(NONE, "/ws"), "c"), "d", order);
  assert.deepEqual(ids(more), ["/folder", "/ws", "c", "d"]);
  assert.equal(more.anchor, "c");
});

test("Shift-click with nothing picked picks that row", () => {
  assert.deepEqual(ids(ranged(NONE, "b", order)), ["b"]);
  assert.equal(ranged(NONE, "gone", order), NONE);
});

test("rows that went away leave the selection", () => {
  const picked = { ...NONE, ids: new Set(["a", "b", "gone"]) };
  assert.deepEqual(ids(kept(picked, order)), ["a", "b"]);
  const same = { ...NONE, ids: new Set(["a"]) };
  assert.equal(kept(same, order), same);
  assert.equal(
    kept({ ...NONE, ids: new Set(["gone"]), mode: true }, order),
    NONE,
  );
});
