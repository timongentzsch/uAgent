import { test } from "node:test";
import assert from "node:assert/strict";
import { refreshUserStyle } from "../src/shared/user-style.ts";

test("stylesheet refreshes ignore stale bodies and retain CSS on failure", async (t) => {
  const requests = [];
  const sheets = new Set();
  t.mock.method(
    globalThis,
    "fetch",
    () => new Promise((resolve, reject) => requests.push({ resolve, reject })),
  );
  const previous = Object.getOwnPropertyDescriptor(globalThis, "document");
  t.after(() => {
    if (previous) Object.defineProperty(globalThis, "document", previous);
    else delete globalThis.document;
  });
  globalThis.document = {
    createElement: () => ({ textContent: "" }),
    head: { append: (sheet) => sheets.add(sheet) },
  };
  const css = () => [...sheets].map((sheet) => sheet.textContent);
  let oldBody;
  const old = refreshUserStyle();
  requests[0].resolve({
    ok: true,
    text: () =>
      new Promise((resolve) => {
        oldBody = resolve;
      }),
  });
  await Promise.resolve();
  const latest = refreshUserStyle();
  requests[1].resolve({ ok: true, text: async () => "new" });
  await latest;
  oldBody("old");
  await old;
  assert.deepEqual(css(), ["new"]);
  for (const fail of [
    (request) => request.reject(new Error("offline")),
    (request) => request.resolve({ ok: false }),
    (request) =>
      request.resolve({
        ok: true,
        text: async () => {
          throw new Error("body lost");
        },
      }),
  ]) {
    const stale = refreshUserStyle();
    const staleRequest = requests.at(-1);
    const failed = refreshUserStyle();
    fail(requests.at(-1));
    await failed;
    staleRequest.resolve({ ok: true, text: async () => "stale" });
    await stale;
    assert.deepEqual(css(), ["new"]);
  }
  const cleared = refreshUserStyle();
  requests.at(-1).resolve({ ok: true, text: async () => "" });
  await cleared;
  assert.deepEqual(css(), [""]);
});
