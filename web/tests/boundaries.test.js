// Where code may reach: a feature stands on shared/ and state/ alone, state/
// is the data layer under every screen, and shared/ is what features have in
// common. An import against that grain couples two screens, or the data to the
// UI, and is moved down to where both can take it instead.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join, normalize } from "node:path";

// Strings are matched first so a "//" inside one is not taken for a comment.
const SCRIPT_TEXT =
  /("(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|`(?:\\.|[^`\\])*`)|\/\/[^\n]*|\/\*[\s\S]*?\*\//g;
const withoutComments = (text, css) =>
  css
    ? text.replace(/\/\*[\s\S]*?\*\//g, "")
    : text.replace(SCRIPT_TEXT, (_, kept) => kept ?? "");

// Relative module paths a source file reaches: import, export-from and
// dynamic import in either quote, and CSS @import as a string or url().
const IMPORT = /(?:from|import)\s*\(?\s*(["'])(\.[^"']*)\1/g;
const CSS_IMPORT =
  /@import\s+(?:url\(\s*(["']?)([^"')\s]+)\1\s*\)|(["'])([^"']+)\3)/g;
function importsOf(text, css = false) {
  const code = withoutComments(text, css);
  return css
    ? [...code.matchAll(CSS_IMPORT)].map((match) => match[2] ?? match[4])
    : [...code.matchAll(IMPORT)].map((match) => match[2]);
}

function* sources(dir, base = "") {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(base, entry.name);
    if (entry.isDirectory()) yield* sources(join(dir, entry.name), path);
    else if (/\.(ts|tsx|css)$/.test(entry.name)) yield path;
  }
}

function* imports() {
  for (const path of sources("src")) {
    const text = readFileSync(join("src", path), "utf8");
    for (const spec of importsOf(text, path.endsWith(".css")))
      yield { path, target: normalize(join(dirname(path), spec)) };
  }
}

test("imports are found in every form, and not in comments", () => {
  const found = (text, css) => importsOf(text, css);
  assert.deepEqual(found(`import x from '../a/b.tsx';`), ["../a/b.tsx"]);
  assert.deepEqual(found(`import "../a.css"; export * from "../b.ts";`), [
    "../a.css",
    "../b.ts",
  ]);
  assert.deepEqual(found("const m = () => import('../c/d.tsx');"), [
    "../c/d.tsx",
  ]);
  assert.deepEqual(found(`// import x from "../a/b.tsx";`), []);
  assert.deepEqual(found(`/* import "../a.css" */ import "./ok.ts";`), [
    "./ok.ts",
  ]);
  assert.deepEqual(found(`const url = "http://x"; import "./ok.ts";`), [
    "./ok.ts",
  ]);
  assert.deepEqual(
    found(
      `@import url("../chat/message.css"); @import url(../x.css); @import '../y.css';`,
      true,
    ),
    ["../chat/message.css", "../x.css", "../y.css"],
  );
  assert.deepEqual(found(`/* @import "../chat/message.css"; */`, true), []);
});

const feature = (path) => path.match(/^features\/([^/]+)\//)?.[1];

function breaches(forbidden) {
  return [...imports()]
    .filter(({ path, target }) => forbidden(path, target))
    .map(({ path, target }) => `${path} imports ${target}`);
}

test("a feature does not import another feature", () => {
  assert.deepEqual(
    breaches((path, target) => {
      const other = feature(target);
      return (
        feature(path) !== undefined &&
        other !== undefined &&
        other !== feature(path)
      );
    }),
    [],
  );
});

test("state imports neither components nor screens", () => {
  assert.deepEqual(
    breaches(
      (path, target) =>
        path.startsWith("state/") &&
        (target.endsWith(".tsx") || /^(features|app)\//.test(target)),
    ),
    [],
  );
});

test("host paths are written in state/api.ts only", () => {
  const stray = [];
  for (const path of sources("src")) {
    if (path === "state/api.ts" || path.endsWith(".css")) continue;
    const code = withoutComments(readFileSync(join("src", path), "utf8"));
    for (const [index, line] of code.split("\n").entries())
      if (line.includes("/api/")) stray.push(`${path}:${index + 1}`);
  }
  assert.deepEqual(stray, []);
});

test("shared imports neither features nor the shell", () => {
  assert.deepEqual(
    breaches(
      (path, target) =>
        path.startsWith("shared/") && /^(features|app)\//.test(target),
    ),
    [],
  );
});
