import test from "node:test";
import assert from "node:assert/strict";
import { boardEdit } from "../src/features/files/board-edit.ts";

const BOARD = `<!doctype html>
<meta name="artboard" content="1600x1000">
<meta name="board">
<title>Flow</title>
<p id="n1" class="note" data-at="10 20">Fields?</p>
<div id=form data-shape=rect data-at="300 200" data-size="180 80">Form</div>
<section id="web" data-at="0 0" data-size="600 400"><h2>Web</h2></section>
<h2 id="t" data-at="10 500">Notes</h2>
<hr data-from="n1" data-to="form">
<hr data-from="form" data-to="web">
`;
const line = (text) => `${text}\n`;
const without = (...texts) =>
  texts.reduce((source, text) => source.replace(line(text), ""), BOARD);
const ITEMS = {
  n1: '<p id="n1" class="note" data-at="10 20">Fields?</p>',
  form: '<div id=form data-shape=rect data-at="300 200" data-size="180 80">Form</div>',
  web: '<section id="web" data-at="0 0" data-size="600 400"><h2>Web</h2></section>',
  t: '<h2 id="t" data-at="10 500">Notes</h2>',
};

test("a move, a resize and a note's text change that item alone, and nothing else of the file", () => {
  const moved = boardEdit(BOARD, { set: [{ id: "n1", at: [40.4, 60] }] });
  assert.equal(moved, BOARD.replace('data-at="10 20"', 'data-at="40 60"'));
  const sized = boardEdit(BOARD, { set: [{ id: "form", size: [200, 90] }] });
  assert.equal(
    sized,
    BOARD.replace('data-size="180 80"', 'data-size="200 90"'),
  );
  // A size the item never had is added to its tag.
  assert.equal(
    boardEdit(BOARD, { set: [{ id: "n1", size: [160, 200] }] }),
    BOARD.replace('data-at="10 20">', 'data-at="10 20" data-size="160 200">'),
  );
  assert.equal(
    boardEdit(BOARD, { set: [{ id: "n1", text: "Which <fields> & why?" }] }),
    BOARD.replace(">Fields?<", ">Which &lt;fields> &amp; why?<"),
  );
});

test("several items moved at once are one change; a shape, a fill, a size of text and a section's name change only those", () => {
  assert.equal(
    boardEdit(BOARD, {
      set: [
        { id: "n1", at: [20, 30] },
        { id: "form", at: [310, 210] },
      ],
    }),
    BOARD.replace('data-at="10 20"', 'data-at="20 30"').replace(
      'data-at="300 200"',
      'data-at="310 210"',
    ),
  );
  assert.equal(
    boardEdit(BOARD, { set: [{ id: "form", shape: "diamond", fill: "blue" }] }),
    BOARD.replace(
      "data-shape=rect data-at",
      'data-shape="diamond" data-at',
    ).replace('data-size="180 80">', 'data-size="180 80" data-fill="blue">'),
  );
  assert.equal(
    boardEdit(BOARD, { set: [{ id: "t", tag: "h1" }] }),
    BOARD.replace(
      '<h2 id="t" data-at="10 500">Notes</h2>',
      '<h1 id="t" data-at="10 500">Notes</h1>',
    ),
  );
  assert.equal(
    boardEdit(BOARD, { set: [{ id: "web", text: "Site" }] }),
    BOARD.replace("<h2>Web</h2>", "<h2>Site</h2>"),
  );
  // A note keeps its tag; text has no shape; a shape takes the format's kinds.
  for (const edit of [
    { id: "n1", tag: "h1" },
    { id: "t", shape: "rect" },
    { id: "t", fill: "blue" },
    { id: "form", shape: "star" },
    { id: "form", fill: "red" },
  ])
    assert.equal(boardEdit(BOARD, { set: [edit] }), null, JSON.stringify(edit));
  // One item the file no longer has: none of them moves.
  assert.equal(
    boardEdit(BOARD, {
      set: [
        { id: "n1", at: [1, 2] },
        { id: "gone", at: [1, 2] },
      ],
    }),
    null,
  );
});

test("an item added goes after the last item, before the arrows, with a new id", () => {
  assert.equal(
    boardEdit(BOARD, {
      add: {
        id: "note1",
        tag: "p",
        note: true,
        fill: "yellow",
        at: [40, 60],
        text: "",
      },
    }),
    BOARD.replace(
      line(ITEMS.t),
      line(ITEMS.t) +
        line(
          '<p id="note1" class="note" data-fill="yellow" data-at="40 60"></p>',
        ),
    ),
  );
  assert.equal(
    boardEdit(BOARD, {
      add: {
        id: "section1",
        tag: "section",
        at: [0, 600],
        size: [400, 240],
        text: "Section",
      },
    }),
    BOARD.replace(
      line(ITEMS.t),
      line(ITEMS.t) +
        line(
          '<section id="section1" data-at="0 600" data-size="400 240"><h2>Section</h2></section>',
        ),
    ),
  );
  assert.match(
    boardEdit(BOARD, {
      add: {
        id: "shape1",
        tag: "div",
        shape: "cloud",
        size: [160, 80],
        at: [1, 2],
        text: "",
      },
    }),
    /<div id="shape1" data-shape="cloud" data-at="1 2" data-size="160 80"><\/div>\n<hr/,
  );
  // An id the file has, or one not lower-case, or an item the format has not.
  for (const add of [
    { id: "n1", tag: "p", at: [1, 2], text: "" },
    { id: "New", tag: "p", at: [1, 2], text: "" },
    { id: "x1", tag: "script", at: [1, 2], text: "" },
    { id: "x1", tag: "div", shape: "star", at: [1, 2], text: "" },
    { id: "x1", tag: "p", fill: "blue", at: [1, 2], text: "" },
  ])
    assert.equal(boardEdit(BOARD, { add }), null, JSON.stringify(add));
});

test("copies of items take new ids and places after the last item; the rest of the file stays", () => {
  assert.equal(
    boardEdit(BOARD, {
      copy: [
        { id: "form", as: "form1", at: [320, 220] },
        { id: "web", as: "web1", at: [20, 20] },
      ],
    }),
    BOARD.replace(
      line(ITEMS.t),
      line(ITEMS.t) +
        line(
          '<div id="form1" data-shape=rect data-at="320 220" data-size="180 80">Form</div>',
        ) +
        line(
          '<section id="web1" data-at="20 20" data-size="600 400"><h2>Web</h2></section>',
        ),
    ),
  );
  assert.equal(
    boardEdit(BOARD, { copy: [{ id: "form", as: "n1", at: [1, 2] }] }),
    null,
  );
  assert.equal(
    boardEdit(BOARD, {
      copy: [
        { id: "form", as: "x1", at: [1, 2] },
        { id: "n1", as: "x1", at: [1, 2] },
      ],
    }),
    null,
  );
});

test("items removed take the arrows that name them; an arrow removed is that line alone", () => {
  assert.equal(
    boardEdit(BOARD, { remove: ["form"] }),
    without(
      ITEMS.form,
      '<hr data-from="n1" data-to="form">',
      '<hr data-from="form" data-to="web">',
    ),
  );
  assert.equal(
    boardEdit(BOARD, { remove: ["n1", "t"] }),
    without(ITEMS.n1, ITEMS.t, '<hr data-from="n1" data-to="form">'),
  );
  assert.equal(boardEdit(BOARD, { remove: ["n1", "gone"] }), null);
  assert.equal(
    boardEdit(BOARD, { unarrow: { from: "form", to: "web" } }),
    without('<hr data-from="form" data-to="web">'),
  );
  assert.equal(
    boardEdit(BOARD, { unarrow: { from: "web", to: "form" } }),
    null,
  );
});

test("an arrow drawn goes after the last arrow, between two items the file has", () => {
  assert.equal(
    boardEdit(BOARD, { arrow: { from: "t", to: "n1" } }),
    `${BOARD}<hr data-from="t" data-to="n1">\n`,
  );
  assert.equal(
    boardEdit(BOARD.replace(/<hr.*\n/g, ""), {
      arrow: { from: "t", to: "n1" },
    }),
    `${BOARD.replace(/<hr.*\n/g, "")}<hr data-from="t" data-to="n1">\n`,
  );
  for (const arrow of [
    { from: "t", to: "t" },
    { from: "t", to: "gone" },
    { from: 't" onclick="x', to: "n1" },
  ])
    assert.equal(boardEdit(BOARD, { arrow }), null, JSON.stringify(arrow));
});

test("items brought to the front go after the last item, sent to the back before the first", () => {
  assert.equal(
    boardEdit(BOARD, { front: ["n1", "form"] }),
    without(ITEMS.n1, ITEMS.form).replace(
      line(ITEMS.t),
      line(ITEMS.t) + line(ITEMS.n1) + line(ITEMS.form),
    ),
  );
  assert.equal(
    boardEdit(BOARD, { back: ["t"] }),
    without(ITEMS.t).replace(line(ITEMS.n1), line(ITEMS.t) + line(ITEMS.n1)),
  );
  assert.equal(boardEdit(BOARD, { back: ["gone"] }), null);
});

test("an item the file no longer has, or an edit a board does not take, changes nothing", () => {
  assert.equal(boardEdit(BOARD, { set: [{ id: "gone", at: [1, 2] }] }), null);
  // An arrow is no item: it has no place.
  assert.equal(
    boardEdit(BOARD.replace("<hr data-from", '<hr id="a" data-from'), {
      set: [{ id: "a", at: [1, 2] }],
    }),
    null,
  );
  assert.equal(boardEdit(BOARD, { set: [{ id: "n1", at: [1] }] }), null);
  assert.equal(
    boardEdit(BOARD, { set: [{ id: "n1", at: [Number.NaN, 2] }] }),
    null,
  );
  assert.equal(
    boardEdit(BOARD, { set: [{ id: 'n1" onclick="x', at: [1, 2] }] }),
    null,
  );
  assert.equal(boardEdit(BOARD, { set: [] }), null);
  assert.equal(boardEdit(BOARD, {}), null);
  // Text goes only where the item holds text alone, or one element that
  // does (a section's name).
  assert.equal(
    boardEdit(BOARD.replace("Fields?", "Fields <b>now</b>"), {
      set: [{ id: "n1", text: "x" }],
    }),
    null,
  );
  assert.equal(
    boardEdit(BOARD.replace("Fields?", "<b>Fields?</b>"), {
      set: [{ id: "n1", text: "x" }],
    }),
    BOARD.replace("Fields?", "<b>x</b>"),
  );
});
