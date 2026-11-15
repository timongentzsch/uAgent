// The Files pane's routes: every way in settles on one view, with Back
// leading where the "Files flow" route table says.
import test from "node:test";
import assert from "node:assert/strict";
import {
  START,
  cite,
  follow,
  fromList,
  opened,
  pop,
  push,
  resolve,
  swap,
} from "../src/features/files/flow.ts";

// A folder in which decks/a and decks/b both hold 01-cover.html.
const tree = {
  "": [
    "README.md",
    "index.html",
    "other.html",
    "decks/",
    "design/",
    "docs/",
    "sheets/",
    "src/",
  ],
  decks: ["a/", "b/"],
  "decks/a": ["01-cover.html", "02-plan.html"],
  "decks/b": ["01-cover.html"],
  design: ["landing/", "system/"],
  "design/landing": [
    "01-hero.html",
    "02-pricing.html",
    "costs.html",
    "style.css",
  ],
  "design/system": ["book.html", "tokens.css"],
  docs: ["book.xlsx", "chart.png", "memo.html", "rows.csv"],
  sheets: ["budget.html", "plan.html"],
  src: ["app.ts", "notes.md"],
};
// What the host says a folder is, and each of its pages.
const kinds = {
  "decks/a": ["deck", "slide"],
  "decks/b": ["deck", "slide"],
  "design/landing": ["design", "page"],
  "design/system": ["system", "doc"],
  docs: ["", "doc"],
  sheets: ["", "sheet"],
};
// A sheet that lies among another folder's pages.
const sheets = ["design/landing/costs.html"];
const files = Object.entries(tree).flatMap(([dir, names]) =>
  names.filter((n) => !n.endsWith("/")).map((n) => (dir ? `${dir}/${n}` : n)),
);
const io = {
  list: async (dir) =>
    tree[dir]
      ? {
          kind: kinds[dir]?.[0] ?? "",
          entries: tree[dir].map((n) => ({
            name: n.replace(/\/$/, ""),
            dir: n.endsWith("/"),
            kind: sheets.includes(`${dir}/${n}`)
              ? "sheet"
              : n.endsWith(".html")
                ? kinds[dir]?.[1]
                : undefined,
          })),
        }
      : null,
  find: async (name) =>
    files.filter((path) => path.split("/").pop() === name.split("/").pop()),
};
// Where a cited path lands, from any state: the state before never matters.
const cited = async (path, lines, from = START) => {
  const fresh = cite({ path, lines }, "/home/me/project");
  assert.deepEqual(fresh.back, []);
  return resolve(from, fresh.view, io);
};
// Every state the pane can be in when a way in is taken: at a list, at a
// canvas, and at a page elsewhere.
const before = [
  START,
  {
    view: { kind: "canvas", dir: "design/landing" },
    back: [{ kind: "list", dir: "design/landing" }],
  },
  {
    view: { kind: "sequence", path: "decks/a/02-plan.html" },
    back: [{ kind: "list", dir: "decks/a" }],
  },
  {
    view: { kind: "file", path: "src/app.ts" },
    back: [{ kind: "list", dir: "src" }],
  },
];
const list = (dir) => ({ kind: "list", dir });
const sequence = (path) => ({ kind: "sequence", path });

// The route table, a test a row ("Every way in" on the design's canvas).
// Rows 5 and 6 (a card for an image or another file) never reach the pane,
// and 14 (a phone) is the same views, checked in the browser.

test("1: a card or Library entry for a deck opens its first slide, Back to the deck's list", async () => {
  for (const from of before)
    assert.deepEqual(await cited("decks/a/", undefined, from), {
      view: sequence("decks/a/01-cover.html"),
      back: [list("decks/a")],
    });
});

test("2: one for a document opens it, Back to its folder's list", async () => {
  for (const from of before)
    assert.deepEqual(await cited("docs/memo.html", undefined, from), {
      view: sequence("docs/memo.html"),
      back: [list("docs")],
    });
});

test("3: one for a design opens its canvas, Back to the design's list", async () => {
  for (const from of before)
    assert.deepEqual(await cited("design/landing/", undefined, from), {
      view: { kind: "canvas", dir: "design/landing" },
      back: [list("design/landing")],
    });
});

test("4: one for a design system opens its brand book, Back to the system's list", async () => {
  for (const from of before)
    for (const path of ["design/system/", "design/system/book.html"])
      assert.deepEqual(await cited(path, undefined, from), {
        view: sequence("design/system/book.html"),
        back: [list("design/system")],
      });
});

test("7: a board on the canvas opens in its sequence, a picture or a film as its file, Back to the canvas", () => {
  const canvas = before[1];
  const board = push(canvas, opened("design/landing/02-pricing.html"));
  assert.deepEqual(board.view, sequence("design/landing/02-pricing.html"));
  assert.deepEqual(pop(board), canvas);
  assert.deepEqual(pop(pop(board)), { view: list("design/landing"), back: [] });
  for (const path of ["design/landing/hero.png", "design/landing/clip.mp4"]) {
    const still = push(canvas, opened(path));
    assert.deepEqual(still.view, {
      kind: "file",
      path,
      lines: undefined,
      source: false,
    });
    assert.deepEqual(pop(still), canvas);
  }
});

test("8: a stack on the canvas opens its document, or its deck at the first slide, Back to the canvas", () => {
  const canvas = before[1];
  // A stack's path is the page it opens at: the document, or a deck's first
  // slide in its folder of the design.
  for (const path of [
    "design/landing/memo.html",
    "design/landing/talk/01-cover.html",
  ]) {
    const stack = push(canvas, opened(path));
    assert.deepEqual(stack.view, sequence(path));
    // Stepping through it replaces; Back is still the canvas.
    assert.deepEqual(pop(swap(stack, sequence(`${path}x`))), canvas);
  }
});

test("9: a step shows another board of the sequence and leaves Back as it was", async () => {
  for (const from of before) {
    const deck = await cited("decks/a/", undefined, from);
    const stepped = swap(deck, sequence("decks/a/02-plan.html"));
    assert.deepEqual(stepped.view, sequence("decks/a/02-plan.html"));
    assert.deepEqual(stepped.back, deck.back);
  }
});

test("10: the tree opens a document, and a deck, design or system from its folder, as a card does", async () => {
  assert.deepEqual(fromList(opened("docs/memo.html")), {
    view: sequence("docs/memo.html"),
    back: [list("docs")],
  });
  // Open on a folder's row is that folder cited.
  for (const from of before)
    for (const folder of ["decks/a/", "design/landing/", "design/system/"])
      assert.deepEqual(
        await resolve(from, cite({ path: folder }, "").view, io),
        await cited(folder),
      );
});

test("11: the tree opens an image on the stage, Back to the list", () => {
  assert.deepEqual(fromList(opened("docs/chart.png")), {
    view: {
      kind: "file",
      path: "docs/chart.png",
      lines: undefined,
      source: false,
    },
    back: [list("docs")],
  });
});

test("12: a reply citing a path with lines opens its source there, Back to its folder's list", async () => {
  for (const from of before) {
    assert.deepEqual(await cited("app.ts", "2-4", from), {
      view: { kind: "file", path: "src/app.ts", lines: [2, 4], source: false },
      back: [list("src")],
    });
    // Markdown and pages have no lines to mark: they open as their source.
    for (const path of ["src/notes.md", "decks/a/01-cover.html"])
      assert.deepEqual(await cited(path, "3", from), {
        view: { kind: "file", path, lines: [3, 3], source: true },
        back: [list(path.slice(0, path.lastIndexOf("/")))],
      });
  }
});

test("a cited folder of one page opens that page; one of none, its list", async () => {
  assert.deepEqual(await cited("decks/b/"), {
    view: sequence("decks/b/01-cover.html"),
    back: [list("decks/b")],
  });
  assert.deepEqual(await cited("src/"), { view: list("src"), back: [] });
});

test("a cited page opens over its design's canvas; a slide and a document have none", async () => {
  assert.deepEqual(await cited("design/landing/01-hero.html"), {
    view: sequence("design/landing/01-hero.html"),
    back: [list("design/landing"), { kind: "canvas", dir: "design/landing" }],
  });
  assert.deepEqual(await cited("decks/a/02-plan.html"), {
    view: sequence("decks/a/02-plan.html"),
    back: [list("decks/a")],
  });
});

test("a sheet opens as itself and counts for no canvas: a folder of sheets is its list", async () => {
  assert.deepEqual(await cited("sheets/"), { view: list("sheets"), back: [] });
  assert.deepEqual(await cited("sheets/budget.html"), {
    view: sequence("sheets/budget.html"),
    back: [list("sheets")],
  });
  // Beside a design's pages it has no canvas above it, as a document.
  assert.deepEqual(await cited("design/landing/costs.html"), {
    view: sequence("design/landing/costs.html"),
    back: [list("design/landing")],
  });
});

test("an Excel or CSV file opens as a sheet; cited at lines, a CSV file is its text", async () => {
  for (const path of ["docs/book.xlsx", "docs/rows.csv"])
    assert.deepEqual(await cited(path), {
      view: sequence(path),
      back: [list("docs")],
    });
  assert.deepEqual((await cited("docs/rows.csv", "2")).view, {
    kind: "file",
    path: "docs/rows.csv",
    lines: [2, 2],
    source: false,
  });
});

test("a bare name several files share asks which, whatever was open", async () => {
  for (const from of before)
    assert.deepEqual(await cited("01-cover.html", undefined, from), {
      view: {
        kind: "pick",
        name: "01-cover.html",
        paths: ["decks/a/01-cover.html", "decks/b/01-cover.html"],
        lines: undefined,
      },
      back: [],
    });
});

test("a file at the top is itself, never searched for", async () => {
  assert.deepEqual((await cited("README.md")).view, {
    kind: "file",
    path: "README.md",
    lines: undefined,
    source: false,
  });
});

test("what is nowhere lands on the top with a note, and leaves nothing behind", async () => {
  for (const path of ["missing/", "nope.txt"])
    assert.deepEqual(await cited(path), {
      view: { kind: "list", dir: "", note: `${path} is not in this folder.` },
      back: [],
    });
});

test("a path outside the folder is said so", () => {
  assert.deepEqual(cite({ path: "/etc/hosts" }, "/home/me/project"), {
    view: { kind: "outside", path: "/etc/hosts" },
    back: [],
  });
  assert.equal(
    cite({ path: "/home/me/project/src/app.ts" }, "/home/me/project").view.path,
    "src/app.ts",
  );
});

test("a cited change opens as its diff", async () => {
  const fresh = cite({ path: "src/app.ts", changes: true }, "/p");
  assert.deepEqual((await resolve(START, fresh.view, io)).view, {
    kind: "file",
    path: "src/app.ts",
    diff: true,
  });
});

test("inside the pane a followed link settles in place, a pick pushes, Back pops to where it was", async () => {
  const opened = before[2];
  // Back with nowhere to go stays.
  assert.deepEqual(pop(START), START);
  // A folder link followed from inside settles in place, Back to here.
  const followed = follow(opened, "design/landing/");
  const settled = await resolve(followed, followed.view, io);
  assert.deepEqual(settled.view, { kind: "canvas", dir: "design/landing" });
  assert.deepEqual(settled.back, [...opened.back, opened.view]);
  // A pick from the matches: Back returns to the matches.
  const pick = await cited("01-cover.html");
  const chosen = follow(pick, "decks/b/01-cover.html");
  const page = await resolve(chosen, chosen.view, io);
  assert.deepEqual(page.view, sequence("decks/b/01-cover.html"));
  assert.deepEqual(pop(page), pick);
});
