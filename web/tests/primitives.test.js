import test from "node:test";
import assert from "node:assert/strict";
import { readdir, readFile } from "node:fs/promises";

// Screens compose shared/ui.tsx primitives; a raw element needs a
// `// raw: <reason>` (or `{/* raw: … */}`) on its line or the one above.
test("features and app use the shared UI primitives", async () => {
  const root = new URL("../src/", import.meta.url);
  const raw =
    /<(?:button|select|input|textarea)\b|class(?:Name)?="dialog-actions"/;
  const justified = /\/[/*]\s*raw:/;
  const violations = [];
  for (const name of await readdir(root, { recursive: true })) {
    if (!/^(?:features|app)\/.*\.tsx$/.test(name)) continue;
    const lines = (await readFile(new URL(name, root), "utf8")).split("\n");
    lines.forEach((line, index) => {
      if (
        raw.test(line) &&
        !justified.test(line) &&
        !justified.test(lines[index - 1] || "")
      )
        violations.push(`${name}:${index + 1}`);
    });
  }
  assert.deepEqual(
    violations,
    [],
    "Use Button, IconButton, Actions or the form primitives from shared/ui.tsx",
  );
});
