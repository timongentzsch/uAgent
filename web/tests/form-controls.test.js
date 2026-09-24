import test from "node:test";
import assert from "node:assert/strict";
import { readdir, readFile } from "node:fs/promises";

test("screens use the shared field primitives", async () => {
  const root = new URL("../src/", import.meta.url);
  const violations = [];
  for (const name of await readdir(root, { recursive: true })) {
    if (!name.endsWith(".tsx") || name === "shared/form-controls.tsx") continue;
    const source = await readFile(new URL(name, root), "utf8");
    for (const match of source.matchAll(/<(?:input|select|textarea)\b/g)) {
      violations.push(
        `${name}:${source.slice(0, match.index).split("\n").length}`,
      );
    }
  }
  assert.deepEqual(
    violations,
    [],
    "Use Input, Select or Textarea so new screens inherit zoom and mobile focus behavior",
  );
});
