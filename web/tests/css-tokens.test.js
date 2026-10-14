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

// Spacing, type and line height come from the scales at the top of
// style.css, so a screen cannot drift a pixel from its neighbours. The
// scales' own definitions (custom properties) are the only place a length
// is written. Markdown headings scale with the prose around them, and the
// remote browser's pointer marker is a fixed-size overlay on a screen
// image: those two sheets are exempt.
const SCALED =
  /^\s*(?:padding|margin|gap|row-gap|column-gap|font-size|line-height)[a-z-]*:\s*([^;]*)/;
const SCALE_EXEMPT = new Set([
  "shared/markdown.css",
  "features/browser/browser.css",
  "showcase.css",
]);
// What may remain once var(...) references are removed: no length with a
// unit, no bare line-height number (1 collapses an icon's box).
const LITERAL = /[\d.]+(?:px|rem|em)\b|(?:^|[\s:])(?:[2-9]|1\.)[\d.]*\s*$/;

test("spacing, font sizes and line heights use the scale tokens", () => {
  const stray = [];
  for (const path of sheets("src")) {
    if (SCALE_EXEMPT.has(path)) continue;
    for (const [index, line] of readFileSync(join("src", path), "utf8")
      .split("\n")
      .entries()) {
      const value = SCALED.exec(line)?.[1].replace(/var\([^)]*\)/g, "");
      if (value && LITERAL.test(value))
        stray.push(`${path}:${index + 1}: ${line.trim()}`);
    }
  }
  assert.deepEqual(stray, []);
});
