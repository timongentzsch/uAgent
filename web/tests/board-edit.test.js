import test from "node:test";
import assert from "node:assert/strict";
import { boardEdit } from "../src/features/files/board-edit.ts";

const BOARD = `<!doctype html>
<meta name="artboard" content="1600x1000">
<meta name="board">
<title>Flow</title>
<p id="n1" class="note" data-at="10 20">Fields?</p>
<div id=form data-shape=rect data-at="300 200" data-size="180 80">Form</div>
<hr data-from="n1" data-to="form">
`;

test("a move, a resize and a note's text change that item alone, and nothing else of the file", () => {
  const moved = boardEdit(BOARD, { id: "n1", at: [40.4, 60] });
  assert.equal(moved, BOARD.replace('data-at="10 20"', 'data-at="40 60"'));
  const sized = boardEdit(BOARD, { id: "form", size: [200, 90] });
  assert.equal(
    sized,
    BOARD.replace('data-size="180 80"', 'data-size="200 90"'),
  );
  // A size the item never had is added to its tag.
  assert.equal(
    boardEdit(BOARD, { id: "n1", size: [160, 200] }),
    BOARD.replace('data-at="10 20">', 'data-at="10 20" data-size="160 200">'),
  );
  assert.equal(
    boardEdit(BOARD, { id: "n1", text: "Which <fields> & why?" }),
    BOARD.replace(">Fields?<", ">Which &lt;fields> &amp; why?<"),
  );
});

test("an item the file no longer has, or an edit a board does not take, changes nothing", () => {
  assert.equal(boardEdit(BOARD, { id: "gone", at: [1, 2] }), null);
  // An arrow is no item: it has no place.
  assert.equal(
    boardEdit(BOARD.replace("<hr data-from", '<hr id="a" data-from'), {
      id: "a",
      at: [1, 2],
    }),
    null,
  );
  assert.equal(boardEdit(BOARD, { id: "n1", at: [1] }), null);
  assert.equal(boardEdit(BOARD, { id: "n1", at: [Number.NaN, 2] }), null);
  assert.equal(boardEdit(BOARD, { id: 'n1" onclick="x', at: [1, 2] }), null);
  // Text goes only where the item holds text alone.
  assert.equal(
    boardEdit(BOARD.replace("Fields?", "<b>Fields?</b>"), {
      id: "n1",
      text: "x",
    }),
    null,
  );
});
