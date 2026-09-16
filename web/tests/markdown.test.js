import test from "node:test";
import assert from "node:assert/strict";
import {
  renderMarkdown,
  renderMarkdownBlocks,
} from "../src/shared/markdown.ts";

const concatenate = (blocks) => blocks.map((block) => block.html).join("");

test("keyed blocks preserve authoritative markdown-it output", async () => {
  const fixtures = [
    "A [late][ref].\n\n- nested\n  - item\n\n[ref]: /safe",
    "| a | b |\n| - | - |\n| 1 | 2 |",
    "> quote\n>\n> continued\n\nparagraph",
    "```js\nconst safe = '<tag>'\n```\n\nafter",
  ];
  for (const fixture of fixtures) {
    assert.equal(
      concatenate(await renderMarkdownBlocks(fixture)),
      await renderMarkdown(fixture),
    );
  }
});

test("appending a tail retains completed block keys and html", async () => {
  const initial = await renderMarkdownBlocks(
    "Completed paragraph.\n\nSecond paragraph",
  );
  const appended = await renderMarkdownBlocks(
    "Completed paragraph.\n\nSecond paragraph grows.",
  );
  assert.equal(appended[0].key, initial[0].key);
  assert.equal(appended[0].html, initial[0].html);
  assert.equal(appended[1].key, initial[1].key);
  assert.notEqual(appended[1].html, initial[1].html);
});

test("late references update only affected parser blocks", async () => {
  const initial = await renderMarkdownBlocks("Independent.\n\nA [late][ref].");
  const defined = await renderMarkdownBlocks(
    "Independent.\n\nA [late][ref].\n\n[ref]: https://example.com",
  );
  assert.equal(defined[0].html, initial[0].html);
  assert.match(defined[1].html, /href="https:\/\/example\.com"/);
  assert.match(defined[1].html, /rel="noopener noreferrer"/);
});

test("unsafe links and images retain the shared security policy", async () => {
  const html = concatenate(
    await renderMarkdownBlocks(
      "[unsafe](javascript:alert(1)) ![tracking](https://example.com/a.png)",
    ),
  );
  assert.doesNotMatch(html, /href="javascript:/);
  assert.doesNotMatch(html, /<img/);
  assert.match(html, /\[image: tracking\]/);
});
