import { test } from "node:test";
import assert from "node:assert/strict";
import { readTokens } from "../src/features/files/tokens.ts";

test("a design system's tokens.css reads back as its colours per theme", () => {
  const tokens = readTokens(`
/* µAgent web */
:root, [data-theme="light"] {
  --page: #fff; /* The ground */
  --ink: #111111; /* Text on page */
  --accent: var(--ink); /* Primary buttons */
  --space-2: 4px; /* Inside controls */
  --radius-0: 0; /* Square corners */
  --font-mono: ui-monospace, monospace; /* Every face */
  --motion: 120ms;
}
[data-theme="dark"] {
  --page: #000;
  --ink: #f5f5f5;
}
@media (pointer: coarse) {
  :root { --space-2: 8px; }
}
.text-body { font: 400 1rem/1.5 var(--font-mono); /* Running text */ }
`);
  assert.deepEqual(tokens.themes, ["light", "dark"]);
  assert.deepEqual(
    tokens.colors.map((token) => [token.name, token.values]),
    [
      ["page", { light: "#fff", dark: "#000" }],
      ["ink", { light: "#111111", dark: "#f5f5f5" }],
      ["accent", { light: "var(--ink)" }],
    ],
  );
});
