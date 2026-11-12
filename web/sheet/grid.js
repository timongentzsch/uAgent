// A tab read once from the page's table as it was written, then drawn over
// it as a spreadsheet's grid.

import { headOf, isHeader, isTotal } from "./book.js";
import { CELL, cellName, letters } from "./text.js";

const attrsOf = (element, skip) =>
  Object.fromEntries(
    [...element.attributes]
      .filter(({ name }) => name !== skip)
      .map(({ name, value }) => [name, value]),
  );

// A tab's charts follow its table with nothing but white space between,
// as the app finds them in the file. A figure left open holds the tables
// after it: it is none.
export function figuresOf(table) {
  const figures = [];
  for (let next = table.nextSibling; next; next = next.nextSibling) {
    if (next.nodeType === Node.TEXT_NODE && !next.data.trim()) continue;
    if (!next.matches?.("figure[data-chart]:not(:has(table))")) break;
    figures.push(next);
  }
  return figures;
}

// Every row as wide as the widest; a header row is padded with header
// cells, so it stays one.
export function readTab(table) {
  const said = [...table.querySelectorAll("col")].flatMap((col) =>
    Array.from({ length: col.span }, () => attrsOf(col, "span")),
  );
  const rows = [...table.rows].map((row) => ({
    attrs: attrsOf(row),
    cells: [...row.cells].map((cell) => ({
      th: cell.localName === "th",
      attrs: attrsOf(cell),
      text: cell.textContent.trim(),
    })),
  }));
  const width = Math.max(
    1,
    said.length,
    ...rows.map((row) => row.cells.length),
  );
  for (const row of rows) {
    const th = row.cells.length > 0 && isHeader(row);
    while (row.cells.length < width)
      row.cells.push({ th, attrs: {}, text: "" });
  }
  return {
    name: table.dataset.sheet,
    attrs: attrsOf(table, "data-sheet"),
    // A "<" written bare in a cell opened a tag: the cell's text is not
    // what was written.
    markup: [...table.querySelectorAll(":is(td, th):has(*)")].map((cell) =>
      cellName({
        row: cell.parentElement.rowIndex + 1,
        column: cell.cellIndex + 1,
      }),
    ),
    columns: Array.from({ length: width }, (_, at) => said[at] ?? {}),
    rows,
    charts: figuresOf(table).map((figure) => ({
      attrs: attrsOf(figure),
      caption: figure.querySelector("figcaption")?.textContent.trim() ?? "",
    })),
  };
}

export const make = (name, props = {}, children = []) => {
  const element = Object.assign(document.createElement(name), props);
  element.append(...children);
  return element;
};
const marked = (element, attrs) => {
  for (const [name, value] of Object.entries(attrs))
    element.setAttribute(name, value);
  return element;
};
const edge = (text, scope, data = {}) => {
  const cell = make("th", {
    className: "sheet-edge",
    textContent: text,
    scope,
  });
  Object.assign(cell.dataset, data);
  return cell;
};

// A text's width on a canvas, asked once a text.
const pen = document.createElement("canvas").getContext("2d");
const known = new Map();
const measured = (text, font) => {
  const key = `${font}\n${text}`;
  if (!known.has(key)) {
    pen.font = font;
    known.set(key, pen.measureText(text).width);
  }
  return known.get(key);
};
// As the grid draws it: every digit as wide as the widest (tabular-nums),
// which a canvas does not do.
const widest = new Map();
const wide = (text, font) => {
  if (!widest.has(font))
    widest.set(
      font,
      [..."0123456789"].reduce((most, digit) =>
        measured(digit, font) > measured(most, font) ? digit : most,
      ),
    );
  return measured(text.replace(/\d/g, widest.get(font)), font);
};
// How many lines a text takes in a cell `room` wide, a word after another.
const lines = (text, font, room) => {
  let [count, used] = [1, 0];
  for (const word of text.split(/\s+/)) {
    const length = wide(`${word} `, font);
    if (used && used + length > room) [count, used] = [count + 1, 0];
    count += Math.max(0, Math.ceil(length / room) - 1);
    used = length > room ? length % room : used + length;
  }
  return count;
};
// A cell's padding and line, as sheet.css sets them; a column is as wide as
// its widest text needs, within these, where the file says no width.
const PAD = 17;
const NARROW = 40;
const WIDE = 400;

// The widths of the table's columns and the heights of its rows, from what
// the cells show: the grid is laid out from these, not by the browser, so
// a column is as wide whatever part of it is drawn.
export function sizesOf(table, book, sheet) {
  const tab = book.tabs[sheet];
  const style = getComputedStyle(table);
  const font = (bold) =>
    `${bold ? 600 : 400} ${style.fontSize} ${style.fontFamily}`;
  // The mark of a header's menu (in a button) and of a list of options.
  const mark = wide("▾", font(false)) + 4;
  const head = headOf(tab);
  const shown = tab.rows.map((row, r) =>
    row.cells.map((_, c) => book.shown(sheet, r + 1, c + 1)),
  );
  // The engine is asked for a cell's style only where a rule can embolden.
  const ruled = tab.columns.map((_, c) => book.emboldens(sheet, c + 1));
  const bold = tab.rows.map(
    (row, r) => (c) =>
      isHeader(row) ||
      isTotal(row) ||
      (ruled[c] && !!book.look(sheet, r + 1, c + 1).bold),
  );
  const widths = tab.columns.map((column, c) => {
    if (column["data-width"]) return Number(column["data-width"]) || NARROW;
    const most = shown.reduce(
      (most, texts, r) =>
        Math.max(
          most,
          wide(texts[c], font(bold[r](c))) +
            (r === head - 1
              ? mark + 4
              : r >= head && column["data-options"]
                ? mark
                : 0),
        ),
      0,
    );
    return Math.min(WIDE, Math.max(NARROW, Math.ceil(most) + PAD));
  });
  // A row is as high as a line of the look the app passes in needs.
  const line = Number.parseFloat(style.lineHeight) || 0;
  const row = Math.max(CELL.height, Math.ceil(line) + 5);
  const wrapped = tab.columns.flatMap((column, c) =>
    /\bwrap\b/.test(column.class ?? "") ? [c] : [],
  );
  // A total row has a line over it.
  const heights = shown.map((texts, r) =>
    Math.max(
      row + (isTotal(tab.rows[r]) ? 1 : 0),
      ...wrapped.map(
        (c) => lines(texts[c], font(bold[r](c)), widths[c] - PAD) * line + 5,
      ),
    ),
  );
  return { widths, heights, row };
}

// Draws the rows and columns `view` names (those held first), each where
// `view.layout` puts it. `filters` is the tab's filtered columns. Windowed,
// the table lies at its first scrolling cell's place inside the grid's area.
// A row is `view.row` high or as high as its wrapped text makes it.
export function draw(table, book, sheet, filters, view) {
  const tab = book.tabs[sheet];
  const { layout, frozen } = view;
  const head = headOf(tab);
  const body = view.rows.map((number) => {
    const r = number - 1;
    const row = tab.rows[r];
    const cells = view.columns.map((at) => {
      const c = at - 1;
      const cell = row?.cells[c];
      if (!cell) {
        const blank = make("td");
        Object.assign(blank.dataset, { row: number, column: at });
        return blank;
      }
      const column = tab.columns[c];
      const shown = book.shown(sheet, number, at);
      const kind = book.kind(sheet, number, at);
      const drawn = marked(make(cell.th ? "th" : "td"), cell.attrs);
      Object.assign(drawn.dataset, { row: number, column: at });
      // A rule's fill lies over the cell's own, and that over its row's.
      const look = book.look(sheet, number, at);
      const fill =
        look.fill ?? cell.attrs["data-fill"] ?? row.attrs["data-fill"];
      if (fill) drawn.dataset.fill = fill;
      drawn.classList.toggle("sheet-bold", !!look.bold);
      drawn.classList.toggle("sheet-strike", !!look.strike);
      drawn.classList.toggle("sheet-number", kind === "number");
      drawn.classList.toggle("sheet-error", kind === "error");
      drawn.classList.toggle("sheet-link", /^=HYPERLINK\(/i.test(cell.text));
      drawn.classList.toggle("sheet-wrap", /\bwrap\b/.test(column.class ?? ""));
      drawn.classList.toggle(
        "sheet-options",
        r >= head && !!column["data-options"],
      );
      if (
        column["data-control"] === "checkbox" &&
        r >= head &&
        kind === "boolean"
      )
        drawn.append(
          make("input", {
            type: "checkbox",
            checked: shown === "TRUE",
            tabIndex: -1,
            ariaLabel: `${letters(at)}${number}`,
          }),
        );
      else drawn.textContent = shown;
      if (r === head - 1)
        drawn.append(
          make("button", {
            type: "button",
            className: "sheet-more",
            tabIndex: -1,
            ariaLabel: `Sort and filter column ${letters(at)}`,
            ariaPressed: filters.has(at),
          }),
        );
      return drawn;
    });
    const drawn = marked(make("tr"), row?.attrs ?? {});
    drawn.classList.toggle("sheet-head", !!row && isHeader(row));
    drawn.style.height = `${view.row + (row && isTotal(row) ? 1 : 0)}px`;
    // Each held row sticks under those before it, each held column beside.
    if (number <= frozen.rows) {
      drawn.classList.add("sheet-frozen");
      drawn.style.setProperty("--sheet-top", `${layout.y(number)}px`);
    }
    drawn.append(edge(number, "row", { row: number }), ...cells);
    return drawn;
  });
  const lettered = make("tr", { className: "sheet-letters" }, [
    edge("", "col"),
    ...view.columns.map((at) => edge(letters(at), "col", { column: at })),
  ]);
  lettered.style.height = `${layout.y(1)}px`;
  const span = (from, to) => `${layout.x(to) - layout.x(from)}px`;
  table.replaceChildren(
    make("colgroup", {}, [
      Object.assign(make("col"), { style: `width:${layout.x(1)}px` }),
      ...view.columns.map((at) =>
        Object.assign(make("col"), { style: `width:${span(at, at + 1)}` }),
      ),
    ]),
    make("thead", {}, [lettered]),
    make("tbody", {}, body),
  );
  view.columns.forEach((at, place) => {
    if (at > frozen.columns) return;
    for (const row of [lettered, ...body]) {
      row.cells[place + 1].classList.add("sheet-pinned");
      row.cells[place + 1].style.left = `${layout.x(at)}px`;
    }
  });
  table.setAttribute("role", "grid");
  const first = (drawn, held) => drawn.find((at) => at > held) ?? held + 1;
  const [row, column] = [
    first(view.rows, frozen.rows),
    first(view.columns, frozen.columns),
  ];
  Object.assign(table.style, {
    width: `${layout.x(1) + view.columns.reduce((sum, at) => sum + layout.x(at + 1) - layout.x(at), 0)}px`,
    left: `${layout.x(column) - layout.x(frozen.columns + 1)}px`,
    top: `${layout.y(row) - layout.y(frozen.rows + 1)}px`,
  });
}

// The cells drawn are marked by where the selection lies: its box and its
// active cell; and those in `read`, the cells a picked chart draws from.
export function paint(table, box, active, read) {
  for (const cell of table.querySelectorAll("tbody [data-column]")) {
    const [row, column] = [
      Number(cell.dataset.row),
      Number(cell.dataset.column),
    ];
    const inside =
      row >= box.top &&
      row <= box.bottom &&
      column >= box.left &&
      column <= box.right;
    cell.classList.toggle("sheet-in", inside);
    cell.classList.toggle(
      "sheet-read",
      !!read &&
        row >= read.top &&
        row <= read.bottom &&
        column >= read.left &&
        column <= read.right,
    );
    if (row === active.row && column === active.column)
      cell.setAttribute("aria-selected", "true");
    else cell.removeAttribute("aria-selected");
  }
}
