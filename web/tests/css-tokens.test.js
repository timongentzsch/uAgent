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

// Fixed sizes come from the size tokens in style.css (--control-s, --pane,
// --popover and the like), so an icon or a control is the same size wherever
// it appears. A hairline is 1px, and 0px is
// zero where a math function needs a length rather than a number. A length
// in em follows the element's own font size, which is the scale there, so em
// is not flagged. Stacking and loop durations are tokens too: a literal
// z-index or animation time is a layer or a speed one screen picked alone.
const SIZED =
  /^\s*(?:width|height|min-width|max-width|min-height|max-height|grid-template-columns|grid-template-rows)\s*:\s*([^;]*)/;
const SIZE_LITERAL = /[\d.]+(?:px|rem)\b/;
const STACKED = /^\s*z-index\s*:\s*([^;]*)/;
const ANIMATED = /^\s*animation(?:-duration)?\s*:\s*([^;]*)/;
const DURATION_LITERAL = /[\d.]+m?s\b/;

test("sizes, stacking and animation times use the scale tokens", () => {
  const stray = [];
  const bare = (value) => value?.replace(/var\([^)]*\)/g, "");
  for (const path of sheets("src")) {
    if (SCALE_EXEMPT.has(path)) continue;
    for (const [index, line] of readFileSync(join("src", path), "utf8")
      .split("\n")
      .entries()) {
      const size = bare(SIZED.exec(line)?.[1])?.replace(/\b[01]px\b/g, "");
      const layer = bare(STACKED.exec(line)?.[1]);
      const time = bare(ANIMATED.exec(line)?.[1]);
      if (
        (size && SIZE_LITERAL.test(size)) ||
        (layer && /\d/.test(layer)) ||
        (time && DURATION_LITERAL.test(time))
      )
        stray.push(`${path}:${index + 1}: ${line.trim()}`);
    }
  }
  assert.deepEqual(stray, []);
});

// A media query cannot read a custom property, so the breakpoints are
// written out in the stylesheets and again where a script asks the same
// question. This pins the set, and that the scripts use the same numbers.
test("breakpoints in the stylesheets match the scripts'", () => {
  const found = new Set();
  for (const path of sheets("src")) {
    const css = readFileSync(join("src", path), "utf8");
    for (const [, kind, width] of css.matchAll(
      /@media[^{]*\((max|min)-width:\s*(\d+)px\)/g,
    ))
      found.add(`${kind}-width: ${width}px`);
  }
  // 601 is the first width the 600px phone layout does not cover.
  assert.deepEqual([...found].sort(), [
    "max-width: 600px",
    "max-width: 900px",
    "min-width: 601px",
  ]);
  const layout = readFileSync("src/shared/layout.ts", "utf8");
  assert.match(layout, /useMedia\("\(max-width: 600px\)"\)/);
  assert.match(layout, /useMedia\("\(max-width: 900px\)"\)/);
});
// Control and row heights and the spacing between and inside boxes are set
// in style.css alone (its .dense list and touch rules included). A sheet
// that redefines one resizes everything inside it, menus too.
const GEOMETRY = /^\s*--(?:control|row|gap|pad|inset-x)\s*:/;
test("only style.css sets the control, row and spacing tokens", () => {
  const stray = [];
  for (const path of sheets("src")) {
    if (path === join("shared", "style.css")) continue;
    for (const [index, line] of readFileSync(join("src", path), "utf8")
      .split("\n")
      .entries())
      if (GEOMETRY.test(line))
        stray.push(`${path}:${index + 1}: ${line.trim()}`);
  }
  assert.deepEqual(stray, []);
});
