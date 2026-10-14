// Where code may reach: a feature stands on shared/ and state/ alone, state/
// is the data layer under every screen, and shared/ is what features have in
// common. An import against that grain couples two screens, or the data to the
// UI, and is moved down to where both can take it instead.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join, normalize } from "node:path";

const IMPORT = /(?:from|import)\s*\(?\s*"(\.[^"]*)"/g;

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
    for (const [, spec] of text.matchAll(IMPORT))
      yield { path, target: normalize(join(dirname(path), spec)) };
  }
}

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

test("shared imports neither features nor the shell", () => {
  assert.deepEqual(
    breaches(
      (path, target) =>
        path.startsWith("shared/") && /^(features|app)\//.test(target),
    ),
    [],
  );
});
