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

const files = [...sheets("src")].map((path) => ({
  path,
  css: readFileSync(join("src", path), "utf8"),
}));

// The `file:line: text` of every line `stray` flags, outside the `exempt`
// sheets. `stray(line, state)` may keep a sheet's own state on `state`.
function strays(stray, exempt = new Set()) {
  const found = [];
  for (const { path, css } of files) {
    if (exempt.has(path)) continue;
    const state = { selector: "" };
    for (const [index, line] of css.split("\n").entries())
      if (stray(line, state))
        found.push(`${path}:${index + 1}: ${line.trim()}`);
  }
  return found;
}

test("colour literals stay in the palettes", () => {
  assert.deepEqual(
    strays((line, state) => {
      if (/^\S.*[{,]$/.test(line)) state.selector += line;
      // Only a palette's custom properties may name a colour.
      const tokens = state.selector.includes(":root") && /^\s*--/.test(line);
      const stray = !tokens && COLOUR.test(line.replace(/\/\*.*?\*\//g, ""));
      if (line.startsWith("}")) state.selector = "";
      return stray;
    }, EXEMPT),
    [],
  );
});

// A divider, a control's edge and the focus ring are --border,
// --border-field and --focus-ring, which each palette makes from its own
// colours.
const EDGE =
  /1px solid var\(--(?:line|field-border)\)|outline:.*var\(--accent\)/;

test("edges and focus rings use their tokens", () => {
  assert.deepEqual(
    strays((line) => !/^\s*--/.test(line) && EDGE.test(line)),
    [],
  );
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
]);
// What may remain once var(...) references are removed: no length with a
// unit, no bare line-height number (1 collapses an icon's box).
const LITERAL = /[\d.]+(?:px|rem|em)\b|(?:^|[\s:])(?:[2-9]|1\.)[\d.]*\s*$/;

test("spacing, font sizes and line heights use the scale tokens", () => {
  assert.deepEqual(
    strays((line) => {
      const value = SCALED.exec(line)?.[1].replace(/var\([^)]*\)/g, "");
      return value && LITERAL.test(value);
    }, SCALE_EXEMPT),
    [],
  );
});

// Fixed sizes come from the size tokens in style.css (--control-s, --pane,
// --popover and the like), so an icon or a control is the same size wherever
// it appears. A hairline is 1px, and 0px is
// zero where a math function needs a length rather than a number. A length
// in em follows the element's own font size, which is the scale there, so em
// is not flagged. Stacking is a token too: a literal z-index is a layer one
// screen picked alone.
const SIZED =
  /^\s*(?:width|height|min-width|max-width|min-height|max-height|grid-template-columns|grid-template-rows)\s*:\s*([^;]*)/;
const SIZE_LITERAL = /[\d.]+(?:px|rem)\b/;
const STACKED = /^\s*z-index\s*:\s*([^;]*)/;

test("sizes and stacking use the scale tokens", () => {
  const bare = (value) => value?.replace(/var\([^)]*\)/g, "");
  assert.deepEqual(
    strays((line) => {
      const size = bare(SIZED.exec(line)?.[1])?.replace(/\b[01]px\b/g, "");
      const layer = bare(STACKED.exec(line)?.[1]);
      return (size && SIZE_LITERAL.test(size)) || (layer && /\d/.test(layer));
    }, SCALE_EXEMPT),
    [],
  );
});

// Every animation and transition takes its times and curves from the motion
// tokens, so one surface cannot move faster or differently from the rest;
// `linear` is a keyword, not a tuned curve. A value may run over lines.
const MOTION = /(?:^|[{;\s])(?:animation|transition)(?:-[a-z-]+)?\s*:([^;}]*)/g;
const MOTION_LITERAL =
  /[\d.]+m?s\b|cubic-bezier\(|steps\(|\bease(?:-in|-out|-in-out)?\b/;

test("animations and transitions use the motion tokens", () => {
  const stray = [];
  for (const { path, css: raw } of files) {
    const css = raw.replace(/\/\*[\s\S]*?\*\//g, (comment) =>
      comment.replace(/[^\n]/g, " "),
    );
    for (const match of css.matchAll(MOTION))
      if (MOTION_LITERAL.test(match[1].replace(/var\([^)]*\)/g, "")))
        stray.push(
          `${path}:${css.slice(0, match.index).split("\n").length}: ${match[0].trim()}`,
        );
  }
  assert.deepEqual(stray, []);
});

// A media query cannot read a custom property, so the breakpoints are
// written out in the stylesheets and again where a script asks the same
// question. This pins the set, and that the scripts use the same numbers.
test("breakpoints in the stylesheets match the scripts'", () => {
  const found = new Set();
  for (const { css } of files)
    for (const [, kind, width] of css.matchAll(
      /@media[^{]*\((max|min)-width:\s*(\d+)px\)/g,
    ))
      found.add(`${kind}-width: ${width}px`);
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
  assert.deepEqual(
    strays(
      (line) => GEOMETRY.test(line),
      new Set([join("shared", "style.css")]),
    ),
    [],
  );
});
