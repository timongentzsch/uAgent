// One list of boards feeds the canvas and the legend: what a folder's pages
// are shown as, in which order, and what they said of themselves.
import test from "node:test";
import assert from "node:assert/strict";
import {
  boardsOf,
  failures,
  hear,
  kindOf,
  pageAt,
  rewritten,
  spotAt,
} from "../src/features/files/boards.ts";

const listing = (kind, ...entries) => ({
  kind,
  entries: entries.map(([name, kind]) => ({ name, dir: false, kind })),
});
const ready = (title, w, h, height = 0) => ({
  type: "ready",
  title,
  artboard: { w, h },
  tweaks: null,
  height,
});
const paths = (boards) => boards.map((board) => board.path);

test("a document is its own in any folder; another page is part of its folder", () => {
  const docs = listing("deck", ["memo.html", "doc"], ["slide.html", "slide"]);
  assert.equal(kindOf(docs, "memo.html"), "doc");
  assert.equal(kindOf(docs, "slide.html"), "deck");
  const system = listing("system", ["book.html", "doc"], ["parts.html"]);
  assert.equal(kindOf(system, "book.html"), "doc");
  assert.equal(kindOf(system, "parts.html"), "system");
  assert.equal(kindOf(listing("", ["a.html"]), "a.html"), "design");
});

test("a deck's slides lie in name order as written, the order its PDF prints", () => {
  const deck = listing(
    "deck",
    ["10-end.html", "slide"],
    ["2-plan.html", "slide"],
    ["01-cover.html", "slide"],
    ["memo.html", "doc"],
    ["style.css"],
  );
  assert.deepEqual(paths(boardsOf("decks/q3", deck, {}, "2-plan.html")), [
    "decks/q3/01-cover.html",
    "decks/q3/10-end.html",
    "decks/q3/2-plan.html",
  ]);
});

test("a slide not drawn yet is as large as one that is, and named by its file", () => {
  const deck = listing("deck", ["a.html", "slide"], ["b.html", "slide"]);
  const seen = hear({}, "b.html", ready("Plan", 1920, 1080));
  assert.deepEqual(
    boardsOf("", deck, seen, "a.html").map(({ title, w, h }) => [title, w, h]),
    [
      ["a.html", 1920, 1080],
      ["Plan", 1920, 1080],
    ],
  );
});

test("a design's pages lie in rows by step, numbers in number order, the stepless first", () => {
  const design = listing(
    "design",
    ["10-end.html", "page"],
    ["2-pricing--annual.html", "page"],
    ["2-pricing.html", "page"],
    ["cover.html"],
    ["memo.html", "doc"],
    ["notes.md"],
  );
  const rows = [
    ["", "cover.html"],
    ["2", "2-pricing--annual.html"],
    ["2", "2-pricing.html"],
    ["10", "10-end.html"],
  ];
  const placed = (boards) => boards.map(({ path, row }) => [row, path]);
  // On the canvas a document lies among them; beside an opened page it is
  // left out, being a sequence of its own.
  assert.deepEqual(placed(boardsOf("", design, {})), [
    rows[0],
    ["", "memo.html"],
    ...rows.slice(1),
  ]);
  assert.deepEqual(placed(boardsOf("", design, {}, "cover.html")), rows);
});

test("a document's boards are its pages, from where print starts each", () => {
  const docs = listing("", ["memo.html", "doc"]);
  let seen = hear({}, "docs/memo.html", ready("Memo", 794, 0, 2500));
  assert.deepEqual(
    boardsOf("docs", docs, seen, "memo.html").map((b) => b.top),
    [0],
  );
  seen = hear(seen, "docs/memo.html", { type: "pages", breaks: [1000, 2000] });
  const pages = boardsOf("docs", docs, seen, "memo.html");
  assert.deepEqual(
    pages.map(({ path, title, w, top, h }) => [path, title, w, top, h]),
    [
      ["docs/memo.html", "Memo", 794, 0, 1000],
      ["docs/memo.html", "Memo", 794, 1000, 1000],
      ["docs/memo.html", "Memo", 794, 2000, 500],
    ],
  );
  // The page a scroll offset is in; at the end, the last, which a short
  // last page would never reach by its top.
  assert.equal(pageAt(pages, 0, false), 0);
  assert.equal(pageAt(pages, 999, false), 1);
  assert.equal(pageAt(pages, 1400, false), 1);
  assert.equal(pageAt(pages, 1700, true), 2);
  // All of it in view: at its end without a scroll, it is at its start.
  assert.equal(pageAt(pages, 0, true), 0);
});

test("what a page says is kept once: its size as it grows, an error however often it is drawn", () => {
  let seen = hear({}, "a.html", ready("A", 1440, 0, 600));
  seen = hear(seen, "a.html", { type: "size", height: 900 });
  assert.equal(seen["a.html"].info.height, 900);
  assert.equal(hear(seen, "a.html", { type: "size", height: 900 }), seen);
  // Before it is ready, a page's error is kept all the same.
  const error = { type: "error", message: "ReferenceError: x", line: 3 };
  seen = hear(hear(seen, "b.html", error), "b.html", error);
  assert.deepEqual(seen["b.html"].errors, ["ReferenceError: x (line 3)"]);
  assert.equal(hear(seen, "b.html", error), seen);
  assert.equal(hear(seen, "a.html", { type: "key", key: "Escape" }), seen);
});

test("failed pages are counted once each and written as lines for the agent", () => {
  const design = listing("design", ["a.html", "page"], ["b.html", "page"]);
  let seen = hear({}, "d/a.html", { type: "error", message: "one", line: 0 });
  seen = hear(seen, "d/a.html", { type: "error", message: "two", line: 7 });
  const boards = boardsOf("d", design, seen);
  assert.deepEqual(
    boards.map((board) => board.failed),
    [true, false],
  );
  assert.deepEqual(failures(boards, seen), {
    count: 1,
    text: "`d/a.html` threw: one; two (line 7)",
  });
  // A document's pages are one file.
  const pages = boardsOf(
    "d",
    listing("", ["a.html", "doc"]),
    hear(seen, "d/a.html", { type: "pages", breaks: [500] }),
    "a.html",
  );
  assert.equal(pages.length, 2);
  assert.equal(failures(pages, seen).count, 1);
  // Written again, the page says anew what fails; a file they share, all.
  assert.deepEqual(rewritten(seen, "d/b.html"), seen);
  assert.deepEqual(rewritten(seen, "d/a.html")["d/a.html"].errors, []);
  assert.deepEqual(rewritten(seen, "")["d/a.html"].errors, []);
});

test("an occurrence is found on its board by the counts before it", () => {
  const counts = [2, 0, 3];
  assert.deepEqual(spotAt(counts, 0), { at: 0, nth: 0 });
  assert.deepEqual(spotAt(counts, 1), { at: 0, nth: 1 });
  assert.deepEqual(spotAt(counts, 2), { at: 2, nth: 0 });
  assert.deepEqual(spotAt(counts, 4), { at: 2, nth: 2 });
  assert.deepEqual(spotAt([], 3), { at: 0, nth: 3 });
});
