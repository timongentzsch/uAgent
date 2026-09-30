import test from "node:test";
import assert from "node:assert/strict";
import { nextIndex, typeAhead } from "../src/shared/listbox-nav.ts";

test("arrows wrap and Home and End jump to the ends", () => {
  assert.equal(nextIndex(-1, "ArrowDown", 3), 0);
  assert.equal(nextIndex(-1, "ArrowUp", 3), 2);
  assert.equal(nextIndex(2, "ArrowDown", 3), 0);
  assert.equal(nextIndex(0, "ArrowUp", 3), 2);
  assert.equal(nextIndex(1, "Home", 3), 0);
  assert.equal(nextIndex(1, "End", 3), 2);
});

test("a letter moves to the next label it starts, wrapping", () => {
  const labels = ["Edit from here", "Fork from here", "Statistics", "Stop"];
  assert.equal(typeAhead(labels, -1, "f"), 1);
  assert.equal(typeAhead(labels, 0, "S"), 2);
  assert.equal(typeAhead(labels, 2, "s"), 3);
  assert.equal(typeAhead(labels, 3, "s"), 2);
  assert.equal(typeAhead(labels, 0, "x"), -1);
  assert.equal(typeAhead(labels, 0, " "), -1);
  assert.equal(typeAhead(labels, 0, "Enter"), -1);
});
