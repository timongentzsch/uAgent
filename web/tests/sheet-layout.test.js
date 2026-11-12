import test from "node:test";
import assert from "node:assert/strict";
import { layoutOf } from "../sheet/layout.js";

test("cells lie where the sizes put them: the table's own, a hidden row none, and past it every cell alike", () => {
  // Columns of 50 and 120 after row numbers 40 wide; rows of 26, one hidden.
  const { x, y, columnAt, rowAt, rect } = layoutOf(
    [50, 120],
    [26, 0, 40],
    40,
    26,
  );
  assert.deepEqual([1, 2, 3, 4, 16384].map(x), [
    40,
    90,
    210,
    306,
    40 + 170 + 16381 * 96,
  ]);
  assert.deepEqual([1, 2, 3, 4, 5, 1048576].map(y), [
    26,
    52,
    52,
    92,
    118,
    92 + 1048572 * 26,
  ]);
  assert.deepEqual(rect({ row: 3, column: 2 }), {
    left: 90,
    top: 52,
    width: 120,
    height: 40,
  });
  // A place is in the cell that starts at or before it; a hidden row has none.
  assert.deepEqual(
    [0, 40, 89, 90, 209, 210, 306, 401].map(columnAt),
    [1, 1, 1, 2, 2, 3, 4, 4],
  );
  assert.deepEqual(
    [0, 26, 51, 52, 91, 92, 117, 118].map(rowAt),
    [1, 1, 1, 3, 3, 4, 4, 5],
  );
  assert.equal(rowAt(y(1048576)), 1048576);
  assert.equal(columnAt(x(16384) + 95), 16384);
});
