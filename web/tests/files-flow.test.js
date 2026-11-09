// The Files pane's routes: every way in settles on one view, with Back
// leading where the "Files flow" route table says.
import test from "node:test";
import assert from "node:assert/strict";
import {
  START,
  cite,
  follow,
  pop,
  push,
  resolve,
  swap,
} from "../src/features/files/flow.ts";

// A folder: decks/a and decks/b both hold 01-cover.html; design/landing has
// two pages, design/system one; src/app.ts sits beside README.md at the top.
const tree = {
  "": ["README.md", "index.html", "other.html", "decks/", "design/", "src/"],
  decks: ["a/", "b/"],
  "decks/a": ["01-cover.html", "02-plan.html"],
  "decks/b": ["01-cover.html"],
  design: ["landing/", "system/"],
  "design/landing": ["01-hero.html", "02-pricing.html", "style.css"],
  "design/system": ["book.html", "tokens.css"],
  src: ["app.ts", "notes.md"],
};
const files = Object.entries(tree).flatMap(([dir, names]) =>
  names.filter((n) => !n.endsWith("/")).map((n) => (dir ? `${dir}/${n}` : n)),
);
const io = {
  list: async (dir) =>
    tree[dir]
      ? {
          entries: tree[dir].map((n) => ({
            name: n.replace(/\/$/, ""),
            dir: n.endsWith("/"),
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
// Every state the pane can be in when a citation arrives.
const before = [
  START,
  {
    view: { kind: "canvas", dir: "decks/a" },
    back: [{ kind: "list", dir: "decks/a" }],
  },
  {
    view: { kind: "page", path: "decks/a/02-plan.html" },
    back: [
      { kind: "list", dir: "decks/a" },
      { kind: "canvas", dir: "decks/a" },
    ],
  },
  {
    view: { kind: "file", path: "src/app.ts" },
    back: [{ kind: "list", dir: "src" }],
  },
];

test("a cited folder of pages opens its canvas, Back to its list, from anywhere", async () => {
  for (const from of before)
    assert.deepEqual(await cited("design/landing/", undefined, from), {
      view: { kind: "canvas", dir: "design/landing" },
      back: [{ kind: "list", dir: "design/landing" }],
    });
});

test("a cited folder of one page opens that page; one of none, its list", async () => {
  assert.deepEqual(await cited("design/system/"), {
    view: { kind: "page", path: "design/system/book.html" },
    back: [{ kind: "list", dir: "design/system" }],
  });
  assert.deepEqual(await cited("src/"), {
    view: { kind: "list", dir: "src" },
    back: [],
  });
});

test("a cited page opens it, Back to its canvas when its folder has one", async () => {
  assert.deepEqual(await cited("decks/a/01-cover.html"), {
    view: { kind: "page", path: "decks/a/01-cover.html" },
    back: [
      { kind: "list", dir: "decks/a" },
      { kind: "canvas", dir: "decks/a" },
    ],
  });
  assert.deepEqual(await cited("decks/b/01-cover.html"), {
    view: { kind: "page", path: "decks/b/01-cover.html" },
    back: [{ kind: "list", dir: "decks/b" }],
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

test("a bare name one file has opens it, at its lines", async () => {
  assert.deepEqual(await cited("app.ts", "2-4"), {
    view: { kind: "file", path: "src/app.ts", lines: [2, 4], source: false },
    back: [{ kind: "list", dir: "src" }],
  });
  // Markdown and pages cited at lines open as their source.
  assert.deepEqual((await cited("src/notes.md", "3")).view, {
    kind: "file",
    path: "src/notes.md",
    lines: [3, 3],
    source: true,
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

test("inside the pane a click pushes, a pick pushes, Back pops to where it was", async () => {
  const canvas = {
    view: { kind: "canvas", dir: "decks/a" },
    back: [{ kind: "list", dir: "decks/a" }],
  };
  const opened = push(canvas, { kind: "page", path: "decks/a/02-plan.html" });
  assert.deepEqual(pop(opened), canvas);
  assert.deepEqual(pop(pop(opened)), {
    view: { kind: "list", dir: "decks/a" },
    back: [],
  });
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
  assert.deepEqual(page.view, { kind: "page", path: "decks/b/01-cover.html" });
  assert.deepEqual(pop(page), pick);
});

test("stepping through slides replaces the page, keeping Back", () => {
  const opened = push(
    { view: { kind: "canvas", dir: "decks/a" }, back: [] },
    { kind: "page", path: "decks/a/01-cover.html" },
  );
  const stepped = swap(opened, { kind: "page", path: "decks/a/02-plan.html" });
  assert.deepEqual(stepped.back, opened.back);
});
