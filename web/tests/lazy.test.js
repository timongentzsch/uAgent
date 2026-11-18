import test from "node:test";
import assert from "node:assert/strict";
import { once, warm } from "../src/shared/lazy.ts";

test("a load is shared, and one that failed is tried again", async () => {
  let calls = 0;
  const load = once(async () => {
    if (++calls === 1) throw new Error("offline");
    return calls;
  });
  await assert.rejects(load(), /offline/);
  assert.equal(await load(), 2);
  assert.equal(await load(), 2);
  assert.equal(calls, 2);
});

test("a warm-up that fails raises nothing", async () => {
  const seen = [];
  const heard = (failure) => seen.push(failure);
  process.on("unhandledRejection", heard);
  warm(Promise.reject(new Error("offline")), Promise.resolve(1));
  await new Promise((done) => setTimeout(done, 10));
  process.off("unhandledRejection", heard);
  assert.deepEqual(seen, []);
});
