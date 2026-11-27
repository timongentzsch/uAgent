// A spec runs in WebKit when its file has the line "// @webkit" (see
// playwright.config.js). A marker spelled any other way would drop the
// spec from WebKit without a word, so none may be.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";

test("every mention of @webkit in a spec is the exact marker line", () => {
  const tests = new URL("./", import.meta.url);
  const strays = readdirSync(tests)
    .filter((name) => name.endsWith(".spec.js"))
    .flatMap((name) =>
      readFileSync(new URL(name, tests), "utf8")
        .split("\n")
        .map((line, index) => [`${name}:${index + 1}`, line])
        .filter(([, line]) => /@webkit/i.test(line) && line !== "// @webkit")
        .map(([where]) => where),
    );
  assert.deepEqual(strays, []);
});
