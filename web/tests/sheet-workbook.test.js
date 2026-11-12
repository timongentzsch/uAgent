import test from "node:test";
import assert from "node:assert/strict";
import { openBook } from "../sheet/book.js";
import { sheetText } from "../sheet/text.js";
import { csvTab, workbookTabs } from "../sheet/workbook.js";
import { ask, calculate, engine } from "./sheet-engine.js";

const LOCALE = ["en", "UTC", "en"];
const texts = (tab) => tab.rows.map((row) => row.cells.map(({ text }) => text));
const heads = (tab) => tab.rows.map((row) => row.cells[0].th);

test("a CSV file is one tab, its rows as wide as the widest, its first row the header", () => {
  const tab = csvTab(engine, "costs", "Item,Net\nRent,1200\nPower\n");
  assert.deepEqual(texts(tab), [
    ["Item", "Net"],
    ["Rent", "1200"],
    ["Power", ""],
  ]);
  assert.deepEqual(heads(tab), [true, false, false]);
  assert.deepEqual([tab.name, tab.columns], ["costs", [{}, {}]]);
  assert.deepEqual(heads(csvTab(engine, "n", "2024,10\n2025,12\n")), [
    true,
    false,
  ]);
  assert.throws(
    () => csvTab(engine, "big", "a\n".repeat(200_001)),
    /big is too large to show/,
  );
});

test("a CSV cell is a number only when it is plainly one: a phone number, an id and a formula stay their text", async () => {
  const data = [
    ["1200", "-12.50", "0.5", "2027-03-01", "true", "Rent, flat", "12:30"],
    ["+1-555-1234", "00123", "1234567890123456789", "-note", "=1+1", "4.5%"],
    ["$1,200", "1e5", "'quoted"],
  ];
  const tab = csvTab(
    engine,
    "data",
    `h\n${data.map((row) => row.map((cell) => `"${cell}"`).join(",")).join("\n")}\n`,
  );
  const book = await openBook(engine, [tab], true, calculate);
  const read = (row) =>
    data[row].map((_, c) => [
      book.shown(0, row + 2, c + 1),
      book.kind(0, row + 2, c + 1),
    ]);
  assert.deepEqual(read(0), [
    ["1200", "number"],
    ["-12.5", "number"],
    ["0.5", "number"],
    ["2027-03-01", "number"],
    ["TRUE", "boolean"],
    ["Rent, flat", "text"],
    ["12:30", "text"],
  ]);
  for (const row of [1, 2])
    assert.deepEqual(
      read(row),
      data[row].map((cell) => [cell, "text"]),
    );
  // Saved as a sheet, such a cell is written with the apostrophe that
  // keeps it text; plain text is written as it is.
  assert.deepEqual(texts(tab).slice(2), [
    [
      "'+1-555-1234",
      "'00123",
      "'1234567890123456789",
      "'-note",
      "'=1+1",
      "'4.5%",
      "",
    ],
    ["'$1,200", "'1e5", "''quoted", "", "", "", ""],
  ]);
  assert.match(
    sheetText("data", [tab]),
    /<td>Rent, flat<td>12:30\n<tr><td>'\+1-555-1234<td>'00123</,
  );
});

test("a CSV file's name makes a tab name a spreadsheet takes", async () => {
  const names = [
    ["exports [2026]: all/rows*?", "exports 2026 allrows"],
    ["a".repeat(40), "a".repeat(31)],
    ["[]", "Sheet"],
  ];
  for (const [file, name] of names) {
    const tab = csvTab(engine, file, "Item\nRent\n");
    assert.equal(tab.name, name);
    assert.equal(
      (await openBook(engine, [tab], true, calculate)).shown(0, 2, 1),
      "Rent",
    );
  }
});

// A workbook as Excel would hold it, from what was typed into each sheet.
const workbook = (sheets, more = () => {}) => {
  const model = new engine.Model("book", ...LOCALE);
  Object.entries(sheets).forEach(([name, rows], sheet) => {
    if (sheet) model.newSheet();
    model.renameSheet(sheet, name);
    rows.forEach((row, r) =>
      row.forEach((text, c) => {
        if (text) model.setUserInput(sheet, r + 1, c + 1, text);
      }),
    );
  });
  more(model);
  return model.toXlsx();
};
const pattern = (model, column, row, height, format) =>
  model.updateRangeStyle(
    { sheet: 0, row, column, width: 1, height },
    "num_fmt",
    format,
  );
const costs = () =>
  workbook(
    {
      Costs: [
        ["Item", "", "Net", "Share"],
        ["Rent", "", "1200", "=C2/C$4"],
        ["Power", "", "90.5", "=C3/C$4"],
        ["Total", "", "=SUM(C2:C3)", "2027-03-01"],
      ],
      Inputs: [
        ["Rate", "4.5%"],
        ["2026", "=Notes!A1"],
      ],
      Notes: [["kept out of sight"]],
    },
    (model) => {
      pattern(model, 3, 2, 3, "#,##0.00");
      pattern(model, 4, 2, 2, "0%");
      model.setColumnsWidth(0, 1, 1, 180);
      model.hideSheet(2);
    },
  );

test("an Excel file is read into tabs: what cells hold, a column's pattern, the widths set, and which sheets are hidden", () => {
  const read = workbookTabs(engine, costs(), "book");
  const [first, inputs] = read;
  assert.deepEqual(
    read.map((tab) => [tab.name, tab.hidden]),
    [
      ["Costs", false],
      ["Inputs", false],
      ["Notes", true],
    ],
  );
  assert.deepEqual(texts(first), [
    ["Item", "", "Net", "Share"],
    ["Rent", "", "1200", "=C2/C$4"],
    ["Power", "", "90.5", "=C3/C$4"],
    ["Total", "", "=SUM(C2:C3)", "2027-03-01"],
  ]);
  // A first row of text, blanks allowed, is the header; one with a number
  // in it is data.
  assert.deepEqual(heads(first), [true, false, false, false]);
  assert.deepEqual(heads(inputs), [false, false]);
  // The pattern every filled cell under the header has is the column's;
  // where they differ each cell keeps its own.
  assert.deepEqual(first.columns, [
    { "data-width": "180" },
    {},
    { "data-format": "#,##0.00" },
    {},
  ]);
  assert.deepEqual(
    first.rows.map((row) => row.cells.map((cell) => cell.attrs)),
    [
      [{}, {}, {}, {}],
      [{}, {}, {}, { "data-format": "0%" }],
      [{}, {}, {}, { "data-format": "0%" }],
      [{}, {}, {}, { "data-format": "yyyy-mm-dd" }],
    ],
  );
  // The engine holds a percentage as a number with its pattern.
  assert.deepEqual(texts(inputs)[0], ["Rate", "0.045"]);
  assert.match(
    inputs.columns[1]["data-format"] ??
      inputs.rows[0].cells[1].attrs["data-format"],
    /%$/,
  );
});

test("a workbook with one cell far out is refused before any cell is built", {
  timeout: 5000,
}, () => {
  const sparse = workbook({ Wide: [["1"]] }, (model) =>
    model.setUserInput(0, 1000, 16384, "x"),
  );
  assert.throws(
    () => workbookTabs(engine, sparse, "sparse"),
    /Wide is too large to show/,
  );
});

test("the tabs read are a book like any: calculated, and locked against every change", async () => {
  const book = await openBook(
    engine,
    workbookTabs(engine, costs(), "book"),
    true,
    calculate,
  );
  assert.equal(book.shown(0, 4, 3), "1,290.50");
  assert.equal(book.shown(0, 2, 4), "93%");
  assert.equal(book.shown(1, 1, 2), "4.50%");
  // A hidden sheet is there for the formulas that read it.
  assert.equal(book.shown(1, 2, 2), "kept out of sight");
  const why = "This file is read-only: save it as a sheet to change it.";
  assert.equal(await book.paste(0, { row: 2, column: 3 }, [["7"]]), why);
  assert.equal(await book.insertRows(0, 2, 1), why);
  assert.equal(await book.sort(0, 3, true), why);
  assert.equal(book.shown(0, 4, 3), "1,290.50");
  assert.equal(book.canUndo(), false);
  // Sent out again it is the same workbook.
  const again = workbookTabs(
    engine,
    await ask({ tabs: book.tabs, xlsx: true }),
    "book",
  );
  const cells = ({ hidden, ...tab }) => tab;
  assert.deepEqual(again.map(cells), structuredClone(book.tabs).map(cells));
});
