// Where a tab's cells lie, by arithmetic alone: a cell that is not drawn
// has its place too, and nothing asks the page for one.

import { CELL } from "./text.js";

// `widths` and `heights` are the table's columns and rows (0 for a hidden
// row); past them a column is CELL.width and a row `row` high, as the
// letters' row is. `edge` is the row numbers' width.
export function layoutOf(widths, heights, edge, row) {
  const ends = (sizes) => {
    const all = [0];
    for (const size of sizes) all.push(all.at(-1) + size);
    return all;
  };
  // Where the `n`th starts, counted from 1.
  const start = (all, unit, lead) => (n) =>
    lead +
    (n <= all.length ? all[n - 1] : all.at(-1) + (n - all.length) * unit);
  // Which one a place lies in: the last that starts at or before it.
  const at = (all, unit, lead) => (place) => {
    const from = place - lead;
    if (from >= all.at(-1))
      return all.length + Math.floor((from - all.at(-1)) / unit);
    let [low, high] = [0, all.length - 1];
    while (low < high) {
      const middle = (low + high + 1) >> 1;
      if (all[middle] <= from) low = middle;
      else high = middle - 1;
    }
    return low + 1;
  };
  const [across, down] = [ends(widths), ends(heights)];
  const x = start(across, CELL.width, edge);
  const y = start(down, row, row);
  return {
    x,
    y,
    columnAt: at(across, CELL.width, edge),
    rowAt: at(down, row, row),
    rect: ({ row, column }) => ({
      left: x(column),
      top: y(row),
      width: x(column + 1) - x(column),
      height: y(row + 1) - y(row),
    }),
  };
}
