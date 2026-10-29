// The look lives in the tokens at the top of style.css: colours, the type,
// spacing and size scales, weights, stripes and motion. Every other
// declaration names a token, so the whole look changes in one place. Values
// are read whole, however many lines they run over.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";

function* tree(dir, extensions, base = "") {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(base, entry.name);
    if (entry.isDirectory())
      yield* tree(join(dir, entry.name), extensions, path);
    else if (extensions.some((end) => entry.name.endsWith(end))) yield path;
  }
}

// Every declaration in the stylesheets: where it is, the selector (and
// at-rules) it sits in, its property and its value.
const declarations = [];
const sheets = [...tree("src", [".css"])].map((path) => ({
  path,
  css: readFileSync(join("src", path), "utf8"),
}));
for (const { path, css: raw } of sheets) {
  const css = raw.replace(/\/\*[\s\S]*?\*\//g, (comment) =>
    comment.replace(/[^\n]/g, " "),
  );
  const rules = [];
  let start = 0;
  for (let at = 0; at < css.length; at++) {
    const char = css[at];
    if (char !== "{" && char !== "}" && char !== ";") continue;
    const text = css.slice(start, at);
    if (char === "{") rules.push(text.trim());
    else {
      const colon = text.indexOf(":");
      if (colon > 0 && text.trim())
        declarations.push({
          where: `${path}:${css.slice(0, start + text.search(/\S/)).split("\n").length}`,
          rule: rules.join(" "),
          property: text.slice(0, colon).trim(),
          value: text.slice(colon + 1).trim(),
        });
      if (char === "}") rules.pop();
    }
    start = at + 1;
  }
}

// A token block: the palettes (the root and the inverse surface) and the
// root's scales. Only a custom property there may hold a literal.
const token = ({ rule, property }) =>
  property.startsWith("--") && /:root|\.inverse/.test(rule);
// The value with every var(...) taken out: what it writes itself.
function bare(value) {
  let out = value;
  while (/var\([^()]*\)/.test(out)) out = out.replace(/var\([^()]*\)/g, "");
  return out;
}
const strays = (stray) =>
  declarations
    .filter((declaration) => !token(declaration) && stray(declaration))
    .map(({ where, property, value }) => `${where}: ${property}: ${value}`);

test("colour literals stay in the palettes", () => {
  const COLOUR =
    /#[0-9a-f]{3,8}\b|\b(?:rgba?|hsla?|oklch|color-mix)\(|^(?:white|black)\b/i;
  assert.deepEqual(
    strays(({ value }) => COLOUR.test(value)),
    [],
  );
});

// Scripts and the page read colours from the palettes too (Mermaid and the
// terminal take them from the computed tokens). The boot script and the
// app manifest run before any stylesheet, so they alone write --bg's value.
test("scripts and the page name no colour", () => {
  const files = [
    ...[...tree("src", [".ts", ".tsx"])].map((path) => join("src", path)),
    "index.html",
  ];
  const found = files.flatMap((path) =>
    [...readFileSync(path, "utf8").matchAll(/["'`]#[0-9a-f]{3,8}["'`]/gi)].map(
      (match) => `${path}: ${match[0]}`,
    ),
  );
  assert.deepEqual(found, []);
});

// A divider, a control's edge and the focus ring are --border,
// --border-field and --focus-ring, which each palette makes from its own
// colours; an accented edge recolours one of them.
test("edges and focus rings use their tokens", () => {
  const EDGE =
    /1px \w+ var\(--(?:line|field-border|accent)(?:-more)?\)|outline:.*var\(--accent\)/;
  assert.deepEqual(
    strays(({ property, value }) => EDGE.test(`${property}: ${value}`)),
    [],
  );
});

// A length written out: px or rem, other than zero and a hairline.
const LENGTH = /(?<![\w.-])(?!0px\b|1px\b)[\d.]+(?:px|rem)\b/;

// Spacing, type and line height come from the scales, so a screen cannot
// drift a pixel from its neighbours: no length, and no bare line-height
// number (1 collapses an icon's box). Markdown headings scale in em with
// the prose around them.
test("spacing, font sizes and line heights use the scale tokens", () => {
  const SCALED =
    /^(?:padding|margin|gap|row-gap|column-gap|font-size|line-height)\b/;
  const LITERAL = /[\d.]+em\b|(?:^|[\s:])(?:[2-9]|1\.)[\d.]*\s*$/;
  const heading = ({ rule, property }) =>
    property === "font-size" &&
    /^\.markdown (?::is\(h1|\[data-level)/.test(rule);
  assert.deepEqual(
    strays((declaration) => {
      if (!SCALED.test(declaration.property) || heading(declaration))
        return false;
      const value = bare(declaration.value);
      return LENGTH.test(value) || LITERAL.test(value);
    }),
    [],
  );
});

// Sizes, places and stripes come from the size tokens (--control-s, --pane,
// --dialog-m, --touch, --stripe and the like), so an icon or a control is
// the same size wherever it appears. A length in em follows the element's
// own font size, which is the scale there, so em is not counted. A custom
// property outside the token blocks is held to the same rule.
test("sizes, positions and border widths use the size tokens", () => {
  const SIZED =
    /^(?:(?:min-|max-)?(?:width|height)|grid-template-\w+|flex(?:-basis)?|top|right|bottom|left|inset(?:-[\w-]+)?|translate|border(?:-[\w-]+)?|outline|--[\w-]+)$/;
  assert.deepEqual(
    strays(
      ({ property, value }) => SIZED.test(property) && LENGTH.test(bare(value)),
    ),
    [],
  );
});

// Weights, letter spacing, the focus ring's offset, an icon's alignment and
// dimming are tokens: --weight-*, --tracking-*, --focus-offset,
// --icon-align and --opacity-disabled. Stacking is a token too: a literal
// z-index is a layer one screen picked alone.
test("weights, tracking, offsets, opacity and stacking use their tokens", () => {
  const WRITTEN = {
    "font-weight": /\d|bold|lighter/,
    "letter-spacing": /\d/,
    "outline-offset": /[\d.]+(?:px|rem|em)\b/,
    "vertical-align": /\d/,
    "z-index": /\d/,
    opacity: /^(?!\s*[01]?\s*$)/,
  };
  assert.deepEqual(
    strays(({ property, value }) => WRITTEN[property]?.test(bare(value))),
    [],
  );
});

// The visible height is --viewport-height, which follows the keyboard and an
// installed app's frame; a scrolling area takes --scroll-s, -m or -l of it.
test("heights come from the viewport and scroll tokens", () => {
  const HEIGHT = /^(?:(?:min-|max-)?height|--[\w-]+)$/;
  assert.deepEqual(
    strays(
      ({ property, value }) =>
        /\d(?:d|s|l)?vh\b/.test(value) ||
        (HEIGHT.test(property) &&
          /--(?:viewport|content)-height\)\s*[*/]/.test(value)),
    ),
    [],
  );
});

// Every animation and transition takes its times and curves from the motion
// tokens, so one surface cannot move faster or differently from the rest;
// `linear` is a keyword, not a tuned curve.
test("animations and transitions use the motion tokens", () => {
  const MOTION_LITERAL =
    /[\d.]+m?s\b|cubic-bezier\(|steps\(|\bease(?:-in|-out|-in-out)?\b/;
  assert.deepEqual(
    strays(
      ({ property, value }) =>
        /^(?:animation|transition)/.test(property) &&
        MOTION_LITERAL.test(bare(value)),
    ),
    [],
  );
});

// A media query cannot read a custom property, so the breakpoints are
// written out in the stylesheets and again where a script asks the same
// question. This pins the set, and that the scripts use the same numbers.
test("breakpoints in the stylesheets match the scripts'", () => {
  const found = new Set();
  for (const { css } of sheets)
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
test("only style.css sets the control, row and spacing tokens", () => {
  assert.deepEqual(
    declarations
      .filter(
        ({ where, property }) =>
          /^--(?:control|row|gap|pad|inset-x)$/.test(property) &&
          !where.startsWith(`${join("shared", "style.css")}:`),
      )
      .map(({ where, property }) => `${where}: ${property}`),
    [],
  );
});
