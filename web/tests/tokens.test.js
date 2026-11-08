import { test } from "node:test";
import assert from "node:assert/strict";
import { readTokens } from "../src/features/files/tokens.ts";

test("a design system's tokens.css reads back as its tokens", () => {
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
    tokens.colors.map((token) => [token.name, token.values, token.usage]),
    [
      ["page", { light: "#fff", dark: "#000" }, "The ground"],
      ["ink", { light: "#111111", dark: "#f5f5f5" }, "Text on page"],
      ["accent", { light: "var(--ink)" }, "Primary buttons"],
    ],
  );
  assert.deepEqual(tokens.scales.space, [
    { name: "space-2", value: "4px", usage: "Inside controls" },
  ]);
  assert.equal(tokens.scales.radius[0].value, "0");
  assert.equal(tokens.scales.font[0].name, "font-mono");
  assert.deepEqual(tokens.text, [
    {
      name: "body",
      value: "font: 400 1rem/1.5 var(--font-mono)",
      usage: "Running text",
    },
  ]);
});
