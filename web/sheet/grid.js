// A tab read once from the page's table as it was written, then drawn over
// it as a spreadsheet's grid.

import { headOf, isHeader, isTotal } from "./book.js";
import { boxAt, CELL, cellName, letters } from "./text.js";

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
// A lane of the groups' gutter, as sheet.css sets it.
export const LANE = 16;
// A row's or column's piece of the gutter: in each group's lane a line
// along it, or the button of the group it follows, which lies over a line.
const gutter = (groups, lanes, n, across) => {
  const marks = Array.from({ length: lanes }, () =>
    make("i", { className: "sheet-lane" }),
  );
  for (const group of groups)
    if (group.at === n) {
      const [first, last] = group.name.split(":");
      const button = make("button", {
        type: "button",
        className: "sheet-fold",
        tabIndex: -1,
        ariaExpanded: `${!group.folded}`,
        ariaLabel: `${across ? "Column" : "Row"}${first === last ? ` ${first}` : `s ${first} to ${last}`}`,
        textContent: group.folded ? "+" : "−",
      });
      button.dataset.group = group.name;
      marks[group.depth - 1] = button;
    } else if (!group.folded && group.from <= n && n <= group.to)
      marks[group.depth - 1].classList.add("sheet-along");
  return lanes ? [make("span", { className: "sheet-lanes" }, marks)] : [];
};

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
  // What is written, and a rule's bold: the engine is asked for a cell's
  // style only where a rule can embolden.
  const ruled = tab.columns.map((_, c) => book.emboldens(sheet, c + 1));
  const marks = tab.rows.map((row, r) =>
    row.cells.map((_, c) => {
      const { bold, wrap } = book.plain(sheet, r + 1, c + 1);
      return {
        wrap,
        bold: bold || (ruled[c] && !!book.look(sheet, r + 1, c + 1).bold),
      };
    }),
  );
  // A merged cell sizes neither its columns nor its rows, as in Excel.
  for (const { top, left } of book.merges(sheet).all)
    if (top <= shown.length && left <= tab.columns.length)
      shown[top - 1][left - 1] = "";
  const widths = tab.columns.map((column, c) => {
    if (column["data-width"]) return Number(column["data-width"]) || NARROW;
    const most = shown.reduce(
      (most, texts, r) =>
        Math.max(
          most,
          wide(texts[c], font(marks[r][c].bold)) +
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
  // A total row has a line over it.
  const heights = shown.map((texts, r) =>
    Math.max(
      row + (isTotal(tab.rows[r]) ? 1 : 0),
      ...texts.map((text, c) =>
        marks[r][c].wrap
          ? lines(text, font(marks[r][c].bold), widths[c] - PAD) * line + 5
          : 0,
      ),
    ),
  );
  return { widths, heights, row };
}

// Draws the rows and columns `view` names (those held first), each where
// `view.layout` puts it. `filters` is the tab's filtered columns. Windowed,
// the table lies at its first scrolling cell's place inside the grid's area.
// A row is `view.row` high or as high as its wrapped text makes it; the
// area starts `view.base` down the grid.
export function draw(table, book, sheet, filters, view) {
  const tab = book.tabs[sheet];
  const { layout, frozen } = view;
  const head = headOf(tab);
  // Each held column sticks beside those before it.
  const pin = (drawn, at) => {
    if (at > frozen.columns) return drawn;
    drawn.classList.add("sheet-pinned");
    drawn.style.left = `${layout.x(at)}px`;
    return drawn;
  };
  // A merged range is drawn as its top left cell over those of its rows and
  // columns drawn, in a part on each side of the held ones. What it holds
  // shows once: in the part of its top left cell, or where that is not
  // drawn, in the first part drawn; the others are empty.
  const merges = book.merges(sheet);
  const partOf = (merge, line, place) => {
    const along = (drawn, from, to) =>
      drawn.filter((n) => n >= from && n <= to);
    const rows = along(view.rows, merge.top, merge.bottom);
    const columns = along(view.columns, merge.left, merge.right);
    const side = (held, at) => (n) => n <= held === at <= held;
    const own = [
      rows.filter(side(frozen.rows, line)),
      columns.filter(side(frozen.columns, place)),
    ];
    const first =
      rows.includes(merge.top) && columns.includes(merge.left)
        ? [merge.top, merge.left]
        : [rows[0], columns[0]];
    return (
      own[0][0] === line &&
      own[1][0] === place && {
        spans: { rowSpan: own[0].length, colSpan: own[1].length },
        shows: line === first[0] && place === first[1],
      }
    );
  };
  const body = view.rows.map((line) => {
    const cells = view.columns.flatMap((place) => {
      const merge = boxAt(merges, { row: line, column: place });
      const part =
        merge.bottom > merge.top || merge.right > merge.left
          ? partOf(merge, line, place)
          : { shows: true };
      if (!part) return [];
      const [number, at] = [merge.top, merge.left];
      const r = number - 1;
      const c = at - 1;
      const cell = tab.rows[r]?.cells[c];
      if (!cell) {
        const blank = pin(Object.assign(make("td"), part.spans), place);
        Object.assign(blank.dataset, { row: number, column: at });
        return [blank];
      }
      const column = tab.columns[c];
      const shown = book.shown(sheet, number, at);
      const kind = book.kind(sheet, number, at);
      const drawn = marked(make(cell.th ? "th" : "td"), cell.attrs);
      Object.assign(drawn.dataset, { row: number, column: at });
      const marks = book.marks(sheet, number, at);
      if (marks.fill) drawn.dataset.fill = marks.fill;
      if (marks.align) drawn.classList.add(`sheet-${marks.align}`);
      for (const name of ["bold", "italic", "strike", "wrap"])
        drawn.classList.toggle(`sheet-${name}`, marks[name]);
      drawn.classList.toggle("sheet-number", kind === "number");
      drawn.classList.toggle("sheet-error", kind === "error");
      drawn.classList.toggle("sheet-link", /^=HYPERLINK\(/i.test(cell.text));
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
      if (!part.shows) drawn.replaceChildren();
      return [pin(Object.assign(drawn, part.spans), place)];
    });
    const row = tab.rows[line - 1];
    const drawn = marked(make("tr"), row?.attrs ?? {});
    drawn.classList.toggle("sheet-head", !!row && isHeader(row));
    drawn.style.height = `${view.row + (row && isTotal(row) ? 1 : 0)}px`;
    // Each held row sticks under those before it, each held column beside.
    if (line <= frozen.rows) {
      drawn.classList.add("sheet-frozen");
      drawn.style.setProperty("--sheet-top", `${layout.y(line)}px`);
    }
    const number = edge(line, "row", { row: line });
    number.append(...gutter(view.outline.rows, view.lanes.rows, line, false));
    drawn.append(number, ...cells);
    return drawn;
  });
  // A column of a table that can be changed has an edge to drag it wider
  // or narrower by.
  const lettered = make("tr", { className: "sheet-letters" }, [
    edge("", "col"),
    ...view.columns.map((at) => {
      const letter = edge(letters(at), "col", { column: at });
      letter.append(
        ...gutter(view.outline.columns, view.lanes.columns, at, true),
      );
      if (at <= tab.columns.length && !view.locked)
        letter.append(make("span", { className: "sheet-resize" }));
      return pin(letter, at);
    }),
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
  table.setAttribute("role", "grid");
  table.style.setProperty("--sheet-gutter-x", `${LANE * view.lanes.rows}px`);
  table.style.setProperty("--sheet-gutter-y", `${LANE * view.lanes.columns}px`);
  const first = (drawn, held) => drawn.find((at) => at > held) ?? held + 1;
  const [row, column] = [
    first(view.rows, frozen.rows),
    first(view.columns, frozen.columns),
  ];
  Object.assign(table.style, {
    width: `${layout.x(1) + view.columns.reduce((sum, at) => sum + layout.x(at + 1) - layout.x(at), 0)}px`,
    left: `${layout.x(column) - layout.x(frozen.columns + 1)}px`,
    top: `${layout.y(row) - layout.y(frozen.rows + 1) - view.base}px`,
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
