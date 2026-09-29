import test from "node:test";
import assert from "node:assert/strict";
import {
  healTail,
  markdownBlocks,
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

test("the unfinished tail of a stream renders formatted, never as raw syntax", () => {
  const cases = [
    ["Some **bold", "Some **bold**"],
    ["a *it", "a *it*"],
    ["***both", "***both***"],
    ["~~gone", "~~gone~~"],
    ["`code", "`code`"],
    ["see [docs](https://ex", "see docs"],
    ["see [docs", "see docs"],
    ["![img](http", ""],
    ["$$\nx^2", "$$\nx^2\n$$"],
    ["Para.\n\nNext **b", "Para.\n\nNext **b**"],
    // A marker just typed waits instead of showing up literally.
    ["done **", "done "],
    // A half table row waits for its line to end.
    ["| a | b |\n|---|---|\n| 1 ", "| a | b |\n|---|---|"],
  ];
  for (const [input, healed] of cases) assert.equal(healTail(input), healed);
  // Left alone: bullets, word-internal underscores, prices, tildes and
  // anything inside an open code fence (markdown-it runs it to the end).
  for (const kept of [
    "* item one",
    "user_name here",
    "cost $5 and",
    "20~25°C",
    "```js\nconst a = '**'",
    // Brackets inside inline code are code, not a half-typed link.
    "Use `arr[i`",
    "A `![x`",
  ])
    assert.equal(healTail(kept), kept);
});

test("healing adds closing markers only and can never introduce HTML", () => {
  for (const input of ["<b>**x", "**<img src=x onerror=1>", "`<script>"]) {
    const html = markdownBlocks(healTail(input))
      .map((block) => block.html)
      .join("");
    assert.doesNotMatch(html, /<(b|img|script)\b/);
  }
});

test("an open code fence streams as a code block, not raw text", () => {
  const [block] = markdownBlocks("```js\nconst a = 1");
  assert.equal(block.code?.language, "js");
  assert.match(block.html, /<pre>/);
});

test("offset parts keep the keys of the whole document", async () => {
  const whole = await renderMarkdownBlocks("One.\n\nTwo.\n\nThree.");
  // As the view splits it: the tail starts on the head's last line.
  const tail = markdownBlocks("\n\nThree.", 2);
  assert.equal(tail[0].key, whole[2].key);
});

test("a long open code block splits above its fence", async () => {
  const { streamingHead } = await import("../src/shared/markdown-split.ts");
  const intro = "Intro paragraph.\n\nSecond paragraph.";
  const fence = "```python\n" + "print('line')\n\n".repeat(60);
  // Blank lines inside the fence never end the head: its fences would not
  // balance. Only the fence's own start does.
  assert.equal(streamingHead(`${intro}\n${fence}`), intro);
  assert.equal(streamingHead("```\nonly code\n\nmore"), "");
});
