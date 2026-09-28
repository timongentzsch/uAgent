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

// The showcase is the living reference: every shared primitive appears in it,
// so a new or changed control is seen there first.
test("the UI showcase covers every shared primitive", async () => {
  const root = new URL("../src/", import.meta.url);
  const showcase = await readFile(new URL("showcase.tsx", root), "utf8");
  // Deferred loads code and draws nothing itself; DialogHeader is drawn by
  // every Modal the showcase opens.
  const drawnElsewhere = new Set(["Deferred", "DialogHeader"]);
  const missing = [];
  for (const file of [
    "shared/ui.tsx",
    "shared/popover.tsx",
    "shared/form-controls.tsx",
    "shared/connection-status.tsx",
  ]) {
    const source = await readFile(new URL(file, root), "utf8");
    for (const [, name] of source.matchAll(/^export function ([A-Z]\w*)/gm))
      if (
        !drawnElsewhere.has(name) &&
        !new RegExp(`\\b${name}\\b`).test(showcase)
      )
        missing.push(`${file}: ${name}`);
  }
  assert.deepEqual(missing, [], "Add these to src/showcase.tsx");
});
