import test from "node:test";
import assert from "node:assert/strict";
import { highlightLines, languageFor } from "../src/shared/highlight.ts";

test("a file's language comes from its name, then its extension", () => {
  assert.equal(languageFor("src/app/app.tsx"), "typescript");
  assert.equal(languageFor("CMakeLists.txt"), "cmake");
  assert.equal(languageFor("deploy/Dockerfile"), "dockerfile");
  assert.equal(languageFor("README.MD"), "markdown");
  assert.equal(languageFor(".gitignore"), "");
  assert.equal(languageFor("LICENSE"), "");
});

test("each highlighted line stands alone", () => {
  const lines = highlightLines("/* one\ntwo */\nint x;\n", "a.cc");
  assert.equal(lines.length, 3);
  // The comment's span closes on its first line and opens again on the next.
  assert.match(lines[0], /^<span class="hljs-comment">\/\* one<\/span>$/);
  assert.match(lines[1], /^<span class="hljs-comment">two \*\/<\/span>$/);
  assert.match(lines[2], /hljs-type/);
});

test("text without a known language is escaped, line by line", () => {
  assert.deepEqual(highlightLines("<a>\n\n&", "notes"), [
    "&lt;a&gt;",
    "",
    "&amp;",
  ]);
  assert.deepEqual(highlightLines("", "notes"), [""]);
});
