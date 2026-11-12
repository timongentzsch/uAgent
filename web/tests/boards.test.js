// One list of boards feeds the canvas and the legend: what a folder's pages
// are shown as, in which order, and what they said of themselves.
import test from "node:test";
import assert from "node:assert/strict";
import {
  boardsOf,
  carried,
  added,
  duplicated,
  failures,
  hear,
  kindOf,
  moved,
  removed,
  rewritten,
  settled,
  shownAfter,
  spotAt,
} from "../src/features/files/boards.ts";

const listing = (kind, ...entries) => ({
  kind,
  entries: entries.map(([name, kind, count]) => ({
    name,
    dir: false,
    kind,
    count,
  })),
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
  // A sheet too.
  const sheets = listing("deck", ["costs.html", "sheet"], ["a.html", "slide"]);
  assert.equal(kindOf(sheets, "costs.html"), "sheet");
  // An Excel or CSV file is a sheet by its name; the host says no kind.
  const read = listing("deck", ["book.xlsx"], ["rows.csv"], ["notes.txt"]);
  assert.deepEqual(
    ["book.xlsx", "rows.csv"].map((name) => kindOf(read, name)),
    ["sheet", "sheet"],
  );
  assert.equal(boardsOf("d", read, {}, "book.xlsx").length, 1);
});

test("a sheet's boards are its tabs: numbered as many as the folder says, then as the sheet names them", () => {
  const folder = listing("", ["budget.html", "sheet", 2], ["a.html", "page"]);
  const tabs = (seen) =>
    boardsOf("d", folder, seen, "budget.html").map((board) => board.title);
  assert.deepEqual(tabs({}), ["1", "2"]);
  const said = { type: "sheet", tabs: ["Costs", "Inputs", "Notes"], hidden: 0 };
  const seen = hear({}, "d/budget.html", said);
  assert.deepEqual(tabs(seen), ["Costs", "Inputs", "Notes"]);
  // Said again, with rows hidden, nothing is new.
  assert.equal(hear(seen, "d/budget.html", { ...said, hidden: 2 }), seen);
  // Every tab is the one file; its cells in error are counted each.
  const errors = ["Costs!B2 =1/0 is #DIV/0!", "Inputs!A1 =x is #NAME?"].reduce(
    (all, message) =>
      hear(all, "d/budget.html", { type: "error", message, line: 0 }),
    seen,
  );
  const boards = boardsOf("d", folder, errors, "budget.html");
  assert.deepEqual(paths(boards), Array(3).fill("d/budget.html"));
  assert.deepEqual(failures(boards, errors, "sheet"), {
    count: 2,
    text: "`d/budget.html` has cells in error: Costs!B2 =1/0 is #DIV/0!; Inputs!A1 =x is #NAME?",
  });
  // On the design's canvas it is a stack of its tabs; in the legend of a
  // page of the design it is skipped.
  const stack = boardsOf("d", folder, seen).find(
    (board) => board.path === "d/budget.html",
  );
  assert.deepEqual(
    [stack.media, stack.type, stack.count],
    ["stack", "sheet", 3],
  );
  assert.equal(boardsOf("d", folder, {})[1].count, 2);
  assert.deepEqual(paths(boardsOf("d", folder, seen, "a.html")), ["d/a.html"]);
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
    ["shot.png"],
  );
  // Beside an opened page lie the folder's pages, and nothing else: a
  // document is a sequence of its own.
  assert.deepEqual(
    boardsOf("", design, {}, "cover.html").map(({ path, row, media }) => [
      row,
      path,
      media,
    ]),
    [
      ["", "cover.html", "page"],
      ["2", "2-pricing--annual.html", "page"],
      ["2", "2-pricing.html", "page"],
      ["10", "10-end.html", "page"],
    ],
  );
});

test("a design's canvas holds its pages, pictures and films, and a stack for a document or a deck in it", () => {
  const design = {
    kind: "design",
    entries: [
      { name: "talk", dir: true },
      { name: "02-talk", dir: true },
      { name: "src", dir: true },
      ...[
        ["02-pricing.html", "page"],
        ["02-chart.svg"],
        ["01-hero.html", "page"],
        ["hero.png"],
        ["torn.gif"],
        ["clip.mp4"],
        ["photo.avif"],
        ["memo.html", "doc"],
        ["cover.html"],
        ["notes.md"],
        ["style.css"],
      ].map(([name, kind]) => ({ name, dir: false, kind, size: 100 })),
      { name: "huge.jpg", dir: false, size: 9 << 20 },
      { name: "02-raw.mov", dir: false, size: 9 << 20 },
    ],
  };
  const deck = listing("deck", ["02.html", "slide"], ["01.html", "slide"]);
  const folders = { talk: deck, "02-talk": deck, src: listing("") };
  // A document has said its pages; a picture its size; another did not load.
  let seen = hear({}, "d/memo.html", ready("Memo", 794, 1123));
  seen = hear(seen, "d/memo.html", { type: "pages", count: 3 });
  seen = hear(seen, "d/hero.png", ready("", 640, 480));
  seen = hear(seen, "d/torn.gif", {
    type: "error",
    message: "did not load",
    line: 0,
  });
  const boards = boardsOf("d", design, seen, undefined, folders);
  // Rows by step, the stepless first; in a row the pages, then the rest.
  assert.deepEqual(
    boards.map(({ row, path, media, count }) => [row, path, media, count]),
    [
      ["", "d/cover.html", "page", undefined],
      ["", "d/memo.html", "stack", 3],
      ["", "d/clip.mp4", "video", undefined],
      ["", "d/hero.png", "image", undefined],
      ["", "d/huge.jpg", "image", undefined],
      ["", "d/photo.avif", "image", undefined],
      ["", "d/talk/01.html", "stack", 2],
      ["", "d/torn.gif", "image", undefined],
      ["01", "d/01-hero.html", "page", undefined],
      ["02", "d/02-pricing.html", "page", undefined],
      ["02", "d/02-chart.svg", "image", undefined],
      ["02", "d/02-raw.mov", "video", undefined],
      ["02", "d/02-talk/01.html", "stack", 2],
    ],
  );
  const named = Object.fromEntries(boards.map((board) => [board.path, board]));
  // A picture is as large as it said; a document's stack is one sheet of its
  // paper; a deck's is named for its folder.
  assert.deepEqual([named["d/hero.png"].w, named["d/hero.png"].h], [640, 480]);
  assert.deepEqual(
    [named["d/memo.html"].title, named["d/memo.html"].h],
    ["Memo", 1123],
  );
  assert.equal(named["d/talk/01.html"].title, "talk");
  // What did not load, or is more than the preview serves, says so by its
  // name and is not the agent's to fix.
  assert.equal(named["d/torn.gif"].failed, "did not load");
  for (const path of ["d/huge.jpg", "d/02-raw.mov"])
    assert.match(named[path].failed, /^is larger than 8/);
  assert.equal(failures(boards, seen).count, 0);
  // Drawn at their final size: what said it, and what will not load.
  assert.deepEqual(
    boards.filter((board) => settled(board, seen)).map((board) => board.path),
    ["d/memo.html", "d/hero.png", "d/huge.jpg", "d/torn.gif", "d/02-raw.mov"],
  );
  // A step that holds no page is still in step order.
  const steps = listing("", ["03-b.html"], ["01-a.html"], ["02-shot.png"]);
  assert.deepEqual(paths(boardsOf("", steps, {})), [
    "01-a.html",
    "02-shot.png",
    "03-b.html",
  ]);
  // Beside one of its pages opened, the pages alone.
  assert.deepEqual(paths(boardsOf("d", design, seen, "cover.html", folders)), [
    "d/cover.html",
    "d/01-hero.html",
    "d/02-pricing.html",
  ]);
  // Before a document has said its pages its stack has no count yet.
  assert.equal(
    boardsOf("d", design, {}).find((board) => board.path === "d/memo.html")
      .count,
    undefined,
  );
});

test("a document's boards are its pages: one sheet of its paper each, as many as it says", () => {
  const docs = listing("", ["memo.html", "doc"]);
  let seen = hear({}, "docs/memo.html", ready("Memo", 794, 1123));
  const pages = () =>
    boardsOf("docs", docs, seen, "memo.html").map(
      ({ path, title, w, h, at }) => [path, title, w, h, at],
    );
  assert.deepEqual(pages(), [["docs/memo.html", "Memo", 794, 1123, 0]]);
  seen = hear(seen, "docs/memo.html", { type: "pages", count: 3 });
  assert.deepEqual(
    pages(),
    [0, 1, 2].map((at) => ["docs/memo.html", "Memo", 794, 1123, at]),
  );
  // Said again, nothing is new; what is no count is one page.
  const again = hear(seen, "docs/memo.html", { type: "pages", count: 3 });
  assert.equal(again, seen);
  seen = hear(seen, "docs/memo.html", { type: "pages", count: "many" });
  assert.equal(pages().length, 1);
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
    ["one", ""],
  );
  assert.deepEqual(failures(boards, seen), {
    count: 1,
    text: "`d/a.html` threw: one; two (line 7)",
  });
  // A document's pages are one file.
  const pages = boardsOf(
    "d",
    listing("", ["a.html", "doc"]),
    hear(seen, "d/a.html", { type: "pages", count: 2 }),
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

const deck = [
  "01-cover.html",
  "02-plan.html",
  "03-numbers.html",
  "04-end.html",
];
const rename = (from, to) => ({ from, to });

test("a slide moved to a new place renames those whose place changed, and no other", () => {
  // Down, up, to the ends: each slide is its place and its own name.
  assert.deepEqual(moved(deck, 0, 1), [
    rename("02-plan.html", "01-plan.html"),
    rename("01-cover.html", "02-cover.html"),
  ]);
  assert.deepEqual(moved(deck, 2, 0), [
    rename("03-numbers.html", "01-numbers.html"),
    rename("01-cover.html", "02-cover.html"),
    rename("02-plan.html", "03-plan.html"),
  ]);
  assert.deepEqual(
    moved(deck, 0, 3).map((move) => move.to),
    ["01-plan.html", "02-numbers.html", "03-end.html", "04-cover.html"],
  );
  assert.deepEqual(moved(deck, 3, 3), []);
  // Names without a number, or with another, get their place.
  assert.deepEqual(moved(["cover.html", "plan.html", "9-end.html"], 2, 1), [
    rename("cover.html", "01-cover.html"),
    rename("9-end.html", "02-end.html"),
    rename("plan.html", "03-plan.html"),
  ]);
});

test("from a hundred slides a place is three digits", () => {
  const hundred = Array.from(
    { length: 100 },
    (_, at) => `${String(at + 1).padStart(2, "0")}-s.html`,
  );
  const moves = moved(hundred, 99, 0);
  assert.equal(moves.length, 100);
  assert.deepEqual(moves[0], rename("100-s.html", "001-s.html"));
  assert.deepEqual(moves[99], rename("99-s.html", "100-s.html"));
  // In name order they are in the new order.
  const names = moves.map((move) => move.to);
  assert.deepEqual([...names].sort(), names);
  // Ninety-nine are two.
  assert.deepEqual(moved(hundred.slice(0, 99), 98, 0)[0].to, "01-s.html");
});

test("a new slide is made under a free name and renamed into the place after its slide", () => {
  assert.deepEqual(added(deck, 1), {
    name: "slide.html",
    moves: [
      rename("slide.html", "03-slide.html"),
      rename("03-numbers.html", "04-numbers.html"),
      rename("04-end.html", "05-end.html"),
    ],
    place: "03-slide.html",
  });
  // Another slide is called so already.
  const twice = added(["01-cover.html", "02-slide.html"], 1);
  assert.equal(twice.name, "slide-2.html");
  assert.equal(twice.place, "03-slide-2.html");
});

test("a duplicate is named for its source and sits right after it", () => {
  assert.deepEqual(duplicated(deck, 1), [
    { from: "02-plan.html", to: "03-plan-copy.html", copy: true },
    rename("03-numbers.html", "04-numbers.html"),
    rename("04-end.html", "05-end.html"),
  ]);
  // A second one of the same slide is numbered, and the first moves on.
  const once = ["01-cover.html", "02-plan.html", "03-plan-copy.html"];
  assert.deepEqual(duplicated(once, 1), [
    { from: "02-plan.html", to: "03-plan-copy-2.html", copy: true },
    rename("03-plan-copy.html", "04-plan-copy.html"),
  ]);
  // Its source may get its place in the same request.
  assert.deepEqual(duplicated(["cover.html"], 0), [
    rename("cover.html", "01-cover.html"),
    { from: "cover.html", to: "02-cover-copy.html", copy: true },
  ]);
});

test("a delete is one move and renames nothing", () => {
  assert.deepEqual(removed(deck, 1), [rename("02-plan.html", "")]);
});

test("what a slide said is carried to its new name, a copy's too, and a deleted one's forgotten", () => {
  let seen = hear({}, "d/01-a.html", ready("A", 1920, 1080));
  seen = hear(seen, "d/02-b.html", ready("B", 1920, 1080));
  seen = hear(seen, "other/02-b.html", ready("Other", 1440, 900));
  const titles = (pages) =>
    Object.fromEntries(
      Object.entries(pages).map(([path, page]) => [path, page.info.title]),
    );
  // Two swap, and one is copied.
  assert.deepEqual(
    titles(
      carried(seen, "d", [
        rename("01-a.html", "02-a.html"),
        rename("02-b.html", "01-b.html"),
        { from: "01-a.html", to: "03-a-copy.html", copy: true },
        rename("03-unseen.html", "04-unseen.html"),
      ]),
    ),
    {
      "d/01-b.html": "B",
      "d/02-a.html": "A",
      "d/03-a-copy.html": "A",
      "other/02-b.html": "Other",
    },
  );
  assert.deepEqual(titles(carried(seen, "d", removed(["02-b.html"], 0))), {
    "d/01-a.html": "A",
    "other/02-b.html": "Other",
  });
});

test("the stage follows its slide to its new name, or to a neighbour when it is deleted", () => {
  assert.equal(shownAfter(moved(deck, 0, 1), deck, 0), "02-cover.html");
  assert.equal(shownAfter(moved(deck, 0, 1), deck, 3), "04-end.html");
  // A copy is not the slide itself.
  assert.equal(shownAfter(duplicated(deck, 1), deck, 1), "02-plan.html");
  assert.equal(shownAfter(removed(deck, 1), deck, 1), "03-numbers.html");
  assert.equal(shownAfter(removed(deck, 3), deck, 3), "03-numbers.html");
  assert.equal(shownAfter(removed(["only.html"], 0), ["only.html"], 0), "");
});
