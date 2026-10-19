import { test } from "node:test";
import assert from "node:assert/strict";
import { safeStorage } from "../src/shared/storage.ts";

const denied = () => {
  throw new DOMException("Storage denied", "SecurityError");
};
const backend = () => {
  const values = new Map();
  return {
    get length() {
      return values.size;
    },
    key: (index) => [...values.keys()][index] ?? null,
    getItem: (key) => values.get(key) ?? null,
    setItem: (key, value) => values.set(key, String(value)),
    removeItem: (key) => values.delete(key),
    clear: () => values.clear(),
  };
};

test("a denied storage getter supports drafts in memory", () => {
  const storage = safeStorage(denied);
  storage.setItem("draft", "unsent");
  assert.equal(storage.getItem("draft"), "unsent");
  assert.equal(storage.length, 1);
  assert.equal(storage.key(0), "draft");
  storage.removeItem("draft");
  assert.equal(storage.getItem("draft"), null);
  storage.setItem("other", "value");
  storage.clear();
  assert.equal(storage.length, 0);
});

test("working storage preserves native reads, writes and other-tab changes", () => {
  const native = backend();
  const storage = safeStorage(() => native);
  storage.setItem("draft", "first");
  assert.equal(native.getItem("draft"), "first");
  native.setItem("draft", "changed elsewhere");
  assert.equal(storage.getItem("draft"), "changed elsewhere");
  native.removeItem("draft");
  assert.equal(storage.getItem("draft"), null);
  storage.setItem("other", "value");
  storage.clear();
  assert.equal(native.length, 0);
});

for (const method of ["getItem", "setItem", "removeItem", "clear"]) {
  test(`a failing storage ${method} retains known values in memory`, () => {
    const native = backend();
    native.setItem("saved", "read before failure");
    const storage = safeStorage(() => native);
    assert.equal(storage.getItem("saved"), "read before failure");
    storage.setItem("draft", "unsent");
    native[method] = denied;
    if (method === "getItem") assert.equal(storage.getItem("draft"), "unsent");
    else if (method === "setItem") storage.setItem("latest", "kept");
    else if (method === "removeItem") storage.removeItem("saved");
    else storage.clear();
    assert.equal(
      storage.getItem("draft"),
      method === "clear" ? null : "unsent",
    );
    if (method === "setItem") assert.equal(storage.getItem("latest"), "kept");
    if (method === "removeItem") assert.equal(storage.getItem("saved"), null);
    storage.setItem("after", "still usable");
    assert.equal(storage.getItem("after"), "still usable");
  });
}
