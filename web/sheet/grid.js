// A tab read once from the page's table as it was written, then drawn over
// it as a spreadsheet's grid.

import { headOf, isHeader } from "./book.js";
import { cellName, frozenOf, letters } from "./text.js";

const attrsOf = (element, skip) =>
  Object.fromEntries(
    [...element.attributes]
      .filter(({ name }) => name !== skip)
      .map(({ name, value }) => [name, value]),
  );

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
  };
}

const make = (name, props = {}, children = []) => {
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

// `filters` is the tab's filtered columns, `hidden` the rows they hide.
export function draw(table, book, sheet, filters, hidden) {
  const tab = book.tabs[sheet];
  const head = headOf(tab);
  const said = frozenOf(tab.attrs["data-frozen"], head);
  const frozen = {
    rows: Math.min(said.rows, tab.rows.length),
    columns: Math.min(said.columns, tab.columns.length),
  };
  const body = tab.rows.map((row, r) => {
    const cells = row.cells.map((cell, c) => {
      const column = tab.columns[c];
      const shown = book.shown(sheet, r + 1, c + 1);
      const kind = book.kind(sheet, r + 1, c + 1);
      const drawn = marked(make(cell.th ? "th" : "td"), cell.attrs);
      Object.assign(drawn.dataset, { row: r + 1, column: c + 1 });
      drawn.classList.toggle("sheet-number", kind === "number");
      drawn.classList.toggle("sheet-error", kind === "error");
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
            ariaLabel: `${letters(c + 1)}${r + 1}`,
          }),
        );
      else drawn.textContent = shown;
      if (r === head - 1)
        drawn.append(
          make("button", {
            type: "button",
            className: "sheet-more",
            tabIndex: -1,
            ariaLabel: `Sort and filter column ${letters(c + 1)}`,
            ariaPressed: filters.has(c + 1),
          }),
        );
      return drawn;
    });
    const drawn = marked(make("tr"), row.attrs);
    drawn.classList.toggle("sheet-head", isHeader(row));
    drawn.classList.toggle("sheet-frozen", r < frozen.rows);
    drawn.hidden = hidden.has(r + 1);
    drawn.append(edge(r + 1, "row", { row: r + 1 }), ...cells);
    return drawn;
  });
  table.replaceChildren(
    make("colgroup", {}, [
      make("col", { className: "sheet-edge" }),
      ...tab.columns.map((column) => {
        const col = make("col");
        if (column["data-width"]) col.style.width = `${column["data-width"]}px`;
        return col;
      }),
    ]),
    make("thead", {}, [
      make("tr", { className: "sheet-letters" }, [
        edge("", "col"),
        ...tab.columns.map((_, c) =>
          edge(letters(c + 1), "col", { column: c + 1 }),
        ),
      ]),
    ]),
    make("tbody", {}, body),
  );
  table.setAttribute("role", "grid");
  // Each held row and column sticks under and beside those before it.
  const tops = body.slice(0, frozen.rows).map((row) => row.offsetTop);
  tops.forEach((top, r) =>
    body[r].style.setProperty("--sheet-top", `${top}px`),
  );
  const lefts = Array.from(
    { length: frozen.columns },
    (_, c) => table.tHead.rows[0].cells[c + 1].offsetLeft,
  );
  for (const row of [table.tHead.rows[0], ...body])
    lefts.forEach((left, c) => {
      row.cells[c + 1].classList.add("sheet-pinned");
      row.cells[c + 1].style.left = `${left}px`;
    });
  // A cell scrolled into view stops short of what is held.
  const root = document.documentElement;
  root.style.scrollPaddingTop = `${body[frozen.rows]?.offsetTop ?? 0}px`;
  root.style.scrollPaddingLeft = `${table.tHead.rows[0].cells[frozen.columns + 1]?.offsetLeft ?? 0}px`;
}

export const cellAt = (table, { row, column }) =>
  table.tBodies[0].rows[row - 1]?.cells[column];

export function paint(table, box, active) {
  for (const cell of table.querySelectorAll(".sheet-in, [aria-selected]")) {
    cell.classList.remove("sheet-in");
    cell.removeAttribute("aria-selected");
  }
  for (let row = box.top; row <= box.bottom; row++)
    for (let column = box.left; column <= box.right; column++)
      cellAt(table, { row, column })?.classList.add("sheet-in");
  cellAt(table, active)?.setAttribute("aria-selected", "true");
}
