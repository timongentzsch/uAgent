// Colours live in the palettes at the top of style.css. A literal anywhere
// else is a colour one theme would get wrong; the remote browser screen is
// the exception, since it always shows a dark desktop.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";

const EXEMPT = new Set(["features/browser/browser.css"]);
const COLOUR =
  /#[0-9a-f]{3,8}\b|\b(?:rgba?|hsla?|oklch|color-mix)\(|:\s*(?:white|black)\b/i;

function* sheets(dir, base = "") {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(base, entry.name);
    if (entry.isDirectory()) yield* sheets(join(dir, entry.name), path);
    else if (entry.name.endsWith(".css")) yield path;
  }
}

test("colour literals stay in the palettes", () => {
  const stray = [];
  for (const path of sheets("src")) {
    if (EXEMPT.has(path)) continue;
    let selector = "";
    for (const [index, line] of readFileSync(join("src", path), "utf8")
      .split("\n")
      .entries()) {
      if (/^\S.*[{,]$/.test(line)) selector += line;
      // Only a palette's custom properties may name a colour.
      const tokens = selector.includes(":root") && /^\s*--/.test(line);
      if (!tokens && COLOUR.test(line.replace(/\/\*.*?\*\//g, "")))
        stray.push(`${path}:${index + 1}: ${line.trim()}`);
      if (line.startsWith("}")) selector = "";
    }
  }
  assert.deepEqual(stray, []);
});
