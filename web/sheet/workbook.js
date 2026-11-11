// An Excel or CSV file read into the tabs a sheet keeps, so it is drawn,
// and saved as a sheet, like one.

import { fromCsv } from "./text.js";

// A file with more cells is not read: the page would not answer for long.
const CELLS = 200_000;
// Excel's last column.
const COLUMNS = 16384;
const TEXT = 2;

// A tab's name as a spreadsheet allows one: 31 characters, none of []:*?/\.
const named = (name) =>
  name
    .replace(/[[\]:*?/\\]/g, "")
    .trim()
    .slice(0, 31) || "Sheet";
const tab = (name, columns, rows) => ({
  name,
  attrs: {},
  markup: [],
  charts: [],
  columns,
  rows,
});
const sized = (name, width, height) => {
  if (width * height > CELLS) throw new Error(`${name} is too large to show`);
};
const cell = (text, attrs = {}) => ({ th: false, attrs, text });

// A number that stays itself when read: no leading zero or "+", and no more
// digits than a spreadsheet keeps. An ISO date, TRUE and FALSE are data too.
const NUMBER = /^-?(0|[1-9]\d*)(\.\d+)?$/;
const isData = (text) =>
  (NUMBER.test(text) && text.replace(/\D/g, "").length <= 15) ||
  /^\d{4}-\d\d-\d\d$/.test(text) ||
  /^(TRUE|FALSE)$/i.test(text);

// A CSV file is data, not what a person types: a cell the engine would read
// as more than its text (a phone number, "=1+1") gets the apostrophe.
export function csvTab(engine, name, text) {
  const lines = fromCsv(text);
  const width = lines.reduce((most, line) => Math.max(most, line.length), 1);
  sized(name, width, lines.length);
  const model = new engine.Model("csv", "en", "UTC", "en");
  model.pauseEvaluation();
  const kept = (said) => {
    if (!said || isData(said)) return said;
    model.setUserInput(0, 1, 1, said);
    return !said.startsWith("'") &&
      model.getCellType(0, 1, 1) === TEXT &&
      model.getCellContent(0, 1, 1) === said
      ? said
      : `'${said}`;
  };
  try {
    return tab(
      named(name),
      Array.from({ length: width }, () => ({})),
      lines.map((line, r) => ({
        attrs: {},
        // The first row of a CSV file is its header.
        cells: Array.from({ length: width }, (_, c) => ({
          ...cell(kept(line[c] ?? "")),
          th: r === 0,
        })),
      })),
    );
  } finally {
    model.free();
  }
}

// Carried: what each cell holds, its number or date pattern, the widths set
// for columns. A hidden sheet is read too: formulas on the others read it.
export function workbookTabs(engine, bytes, name) {
  const model = engine.Model.fromXlsx(bytes, name, "en", "UTC", "en");
  const read = (sheet, at) => {
    let [width, height] = [1, 1];
    for (let column = 1; column <= COLUMNS; column++)
      for (const row of model.getRowsWithData(at, column))
        [width, height] = [column, Math.max(height, row)];
    sized(sheet.name, width, height);
    const usual = model.getColumnWidth(at, COLUMNS);
    const columns = Array.from({ length: width }, (_, c) => {
      const wide = model.getColumnWidth(at, c + 1);
      return wide === usual ? {} : { "data-width": `${Math.round(wide)}` };
    });
    const rows = Array.from({ length: height }, (_, r) => ({
      attrs: {},
      cells: Array.from({ length: width }, (_, c) => {
        const format = model.getCellStyle(at, r + 1, c + 1).style.num_fmt;
        return cell(
          model.getCellContent(at, r + 1, c + 1),
          format === "general" ? {} : { "data-format": format },
        );
      }),
    }));
    // A first row whose filled cells are all text is the header.
    const filled = rows[0].cells.flatMap(({ text }, c) =>
      text ? [c + 1] : [],
    );
    const headed =
      filled.length > 0 &&
      filled.every((column) => model.getCellType(at, 1, column) === TEXT);
    if (headed) for (const head of rows[0].cells) head.th = true;
    // A pattern every filled cell of a column shares is the column's.
    columns.forEach((column, c) => {
      const body = rows.slice(headed ? 1 : 0).map((row) => row.cells[c]);
      const [format, ...others] = new Set(
        body.filter(({ text }) => text).map((one) => one.attrs["data-format"]),
      );
      if (!format || others.length) return;
      column["data-format"] = format;
      for (const { cells } of rows)
        if (cells[c].attrs["data-format"] === format) cells[c].attrs = {};
    });
    return {
      ...tab(sheet.name, columns, rows),
      hidden: sheet.state !== "visible",
    };
  };
  try {
    return model.getWorksheetsProperties().map(read);
  } finally {
    model.free();
  }
}
