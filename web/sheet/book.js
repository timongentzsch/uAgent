// A sheet's tabs as its file keeps them. What was typed is the record: the
// engine reads "4.5%" as 0.045, so it only calculates and moves formulas.

import { boxName, inOrder, letters, moved, tableText } from "./text.js";

// An engine sheet no tab shows, for sort keys and a selection's sum.
const SCRATCH = "⌁";
// Fifteen digits, so large numbers sort and sum exactly.
const PLAIN = "0.00000000000000E+00";
const KINDS = { 1: "number", 4: "boolean", 16: "error" };
const STEPS = 100;

const blank = (th = false) => ({ th, attrs: {}, text: "" });
export const isHeader = (row) => row.cells.every((cell) => cell.th);
export const isTotal = (row) => /\btotal\b/.test(row.attrs.class ?? "");
// A summary may sit above its table: the header ends at its last row.
export const headOf = (tab) => tab.rows.findLastIndex(isHeader) + 1;
// A cell with an element in it was not read as written ("<" opens a tag).
const markupOf = (tab) =>
  tab.markup.map((ref) => `${tab.name}!${ref} holds markup: write < as &lt;`);

const eachCell = (tabs, visit) =>
  tabs.forEach((tab, sheet) =>
    tab.rows.forEach((row, r) =>
      row.cells.forEach((cell, c) => visit(cell, sheet, r + 1, c + 1)),
    ),
  );
// A tab or a row left alone stays the same object: a step holds only what
// it changed.
const mapCells = (tabs, change) =>
  tabs.map((tab, sheet) => {
    const rows = tab.rows.map((row, r) => {
      const cells = row.cells.map((cell, c) =>
        change(cell, sheet, r + 1, c + 1),
      );
      return cells.some((cell, c) => cell !== row.cells[c])
        ? { ...row, cells }
        : row;
    });
    return rows.some((row, r) => row !== tab.rows[r]) ? { ...tab, rows } : tab;
  });
const withText = (cell, text) =>
  cell.text === text ? cell : { ...cell, text };

export function openBook(engine, written) {
  let tabs = written;
  let model;
  const past = [];
  const future = [];

  const build = () => {
    model?.free();
    model = new engine.Model("Sheet", "en", "UTC", "en");
    model.pauseEvaluation();
    // Named as it is made: a later tab may bear the next default name.
    tabs.forEach((tab, sheet) => {
      if (sheet) model.newSheet();
      model.renameSheet(sheet, tab.name);
    });
    model.newSheet();
    model.renameSheet(tabs.length, SCRATCH);
    tabs.forEach((tab, sheet) => {
      const format = (pattern, row, column, height) => {
        if (pattern && height)
          model.updateRangeStyle(
            { sheet, row, column, width: 1, height },
            "num_fmt",
            pattern,
          );
      };
      // Formats before inputs: a formula shows as the cells it reads do.
      tab.columns.forEach((column, c) =>
        format(column["data-format"], 1, c + 1, tab.rows.length),
      );
      tab.rows.forEach((row, r) =>
        row.cells.forEach((cell, c) =>
          format(cell.attrs["data-format"], r + 1, c + 1, 1),
        ),
      );
    });
    eachCell(tabs, (cell, sheet, row, column) => {
      if (cell.text) model.setUserInput(sheet, row, column, cell.text);
    });
    model.resumeEvaluation();
    model.evaluate();
  };
  build();

  // True for a step made, false for none, or why it was refused.
  const commit = (next) => {
    const changed = next.filter((tab, at) => tab !== tabs[at]);
    if (!changed.length) return false;
    const [refused] = changed.flatMap(markupOf);
    if (refused) {
      // What led here may have moved the engine's rows already.
      build();
      return refused;
    }
    past.push(tabs);
    if (past.length > STEPS) past.shift();
    future.length = 0;
    tabs = next;
    build();
    return true;
  };
  const step = (from, to) => {
    if (!from.length) return false;
    to.push(tabs);
    tabs = from.pop();
    build();
    return true;
  };
  const edit = (sheet, change) =>
    tabs.map((tab, at) => (at === sheet ? change(tab) : tab));
  // Rows or columns moved by the engine. A formula takes the engine's text
  // only where the move changed it: the engine rewrites "=1e3" as "=1000".
  const restructured = (move, next) => {
    const was = new Map();
    eachCell(tabs, (cell, sheet, row, column) => {
      if (cell.text.startsWith("="))
        was.set(cell, model.getCellContent(sheet, row, column));
    });
    move();
    return commit(
      mapCells(next, (cell, sheet, row, column) => {
        const now = was.has(cell) && model.getCellContent(sheet, row, column);
        return now && now !== was.get(cell) ? withText(cell, now) : cell;
      }),
    );
  };
  const where = (name, box) => `${engine.quoteName(name)}!${boxName(box)}`;
  const worked = (formulas) => {
    if (!formulas.length) return [];
    const scratch = tabs.length;
    model.pauseEvaluation();
    formulas.forEach((formula, at) =>
      model.setUserInput(scratch, at + 1, 1, formula),
    );
    model.updateRangeStyle(
      { sheet: scratch, row: 1, column: 1, width: 1, height: formulas.length },
      "num_fmt",
      PLAIN,
    );
    model.resumeEvaluation();
    model.evaluate();
    return formulas.map((_, at) => {
      const kind = KINDS[model.getCellType(scratch, at + 1, 1)] ?? "text";
      const shown = model.getFormattedCellValue(scratch, at + 1, 1);
      return { kind, value: kind === "number" ? Number(shown) : shown };
    });
  };
  const spans = (formula) =>
    engine
      .getTokens(formula)
      .filter(({ token }) => token.Reference || token.Range);
  // The first formula that could not be moved, which stops the change.
  let stuck;
  const relocated = (text, by, own) => {
    if (!text.startsWith("=")) return text;
    const formula = moved(text, spans(text), by, own);
    if (formula === null) stuck ??= text;
    return formula ?? text;
  };
  const placed = (what, next) => {
    const formula = stuck;
    stuck = undefined;
    return formula
      ? `Not ${what}: ${formula} cannot be moved with its cell.`
      : commit(next);
  };

  return {
    get tabs() {
      return tabs;
    },
    canUndo: () => past.length > 0,
    canRedo: () => future.length > 0,
    undo: () => step(past, future),
    redo: () => step(future, past),
    table: (sheet) => tableText(tabs[sheet]),
    shown: (sheet, row, column) =>
      model.getFormattedCellValue(sheet, row, column),
    kind: (sheet, row, column) =>
      KINDS[model.getCellType(sheet, row, column)] ?? "text",

    errors() {
      const found = tabs.flatMap(markupOf);
      eachCell(tabs, (cell, sheet, row, column) => {
        if (model.getCellType(sheet, row, column) === 16)
          found.push(
            `${tabs[sheet].name}!${letters(column)}${row} ${cell.text} is ${model.getFormattedCellValue(sheet, row, column)}`,
          );
      });
      return found.slice(0, 20);
    },

    // Nothing for fewer than two numbers, or with a cell in error.
    summary(sheet, box) {
      const range = where(tabs[sheet].name, box);
      const [count, sum] = worked([`=COUNT(${range})`, `=SUM(${range})`]);
      return count.value > 1 && sum.kind === "number"
        ? {
            count: count.value,
            sum: sum.value,
            average: sum.value / count.value,
          }
        : undefined;
    },

    // Text set down from a cell on, as far as the tab reaches. From this
    // book's own cells (`from`) formulas move along, and a cut clears.
    paste(sheet, at, rows, from) {
      const texts = new Map();
      if (from?.cut && from.sheet === sheet)
        for (let row = from.box.top; row <= from.box.bottom; row++)
          for (let column = from.box.left; column <= from.box.right; column++)
            texts.set(`${row},${column}`, "");
      const by = from &&
        !from.cut && {
          row: at.row - from.box.top,
          column: at.column - from.box.left,
        };
      rows.forEach((cells, r) =>
        cells.forEach((text, c) =>
          texts.set(
            `${at.row + r},${at.column + c}`,
            by ? relocated(text, by) : text,
          ),
        ),
      );
      return placed(
        "pasted",
        mapCells(tabs, (cell, on, row, column) =>
          on === sheet && texts.has(`${row},${column}`)
            ? withText(cell, texts.get(`${row},${column}`))
            : cell,
        ),
      );
    },

    // One row alone takes the row above it.
    fillDown(sheet, box) {
      const top = box.bottom > box.top ? box.top : box.top - 1;
      if (top < 1) return false;
      const source = tabs[sheet].rows[top - 1].cells;
      return placed(
        "filled",
        mapCells(tabs, (cell, at, row, column) =>
          at === sheet &&
          row > top &&
          row <= box.bottom &&
          column >= box.left &&
          column <= box.right
            ? withText(
                cell,
                relocated(source[column - 1].text, {
                  row: row - top,
                  column: 0,
                }),
              )
            : cell,
        ),
      );
    },

    insertRows: (sheet, at, count) =>
      restructured(
        () => model.insertRows(sheet, at, count),
        edit(sheet, (tab) => ({
          ...tab,
          rows: tab.rows.toSpliced(
            at - 1,
            0,
            ...Array.from({ length: count }, () => ({
              attrs: {},
              cells: tab.columns.map(() => blank()),
            })),
          ),
        })),
      ),
    deleteRows: (sheet, at, count) =>
      count < tabs[sheet].rows.length &&
      restructured(
        () => model.deleteRows(sheet, at, count),
        edit(sheet, (tab) => ({
          ...tab,
          rows: tab.rows.toSpliced(at - 1, count),
        })),
      ),
    insertColumns: (sheet, at, count) =>
      restructured(
        () => model.insertColumns(sheet, at, count),
        edit(sheet, (tab) => ({
          ...tab,
          columns: tab.columns.toSpliced(
            at - 1,
            0,
            ...Array.from({ length: count }, () => ({})),
          ),
          // A header row stays one: its new cells are headers too.
          rows: tab.rows.map((row) => ({
            ...row,
            cells: row.cells.toSpliced(
              at - 1,
              0,
              ...Array.from({ length: count }, () => blank(isHeader(row))),
            ),
          })),
        })),
      ),
    deleteColumns: (sheet, at, count) =>
      count < tabs[sheet].columns.length &&
      restructured(
        () => model.deleteColumns(sheet, at, count),
        edit(sheet, (tab) => ({
          ...tab,
          columns: tab.columns.toSpliced(at - 1, count),
          rows: tab.rows.map((row) => ({
            ...row,
            cells: row.cells.toSpliced(at - 1, count),
          })),
        })),
      ),

    // Under the header and but for total rows; a formula follows its row.
    sort(sheet, column, descending) {
      const tab = tabs[sheet];
      const places = tab.rows
        .map((_, at) => at)
        .filter((at) => at >= headOf(tab) && !isTotal(tab.rows[at]));
      const values = worked(
        places.map(
          (at) =>
            `=${where(tab.name, { top: at + 1, left: column, bottom: at + 1, right: column })}`,
        ),
      );
      const sorted = places
        .map((at, n) => ({
          at,
          key: tab.rows[at].cells[column - 1].text
            ? values[n]
            : { kind: "empty" },
        }))
        .sort((a, b) => inOrder(a.key, b.key, descending) || a.at - b.at);
      const rows = [...tab.rows];
      places.forEach((to, n) => {
        const from = sorted[n].at;
        if (from !== to)
          rows[to] = {
            ...tab.rows[from],
            cells: tab.rows[from].cells.map((cell) =>
              withText(
                cell,
                relocated(
                  cell.text,
                  { row: to - from, column: 0 },
                  { row: from + 1, tab: tab.name },
                ),
              ),
            ),
          };
      });
      return placed(
        "sorted",
        rows.every((row, at) => row === tab.rows[at])
          ? tabs
          : edit(sheet, () => ({ ...tab, rows })),
      );
    },
  };
}
