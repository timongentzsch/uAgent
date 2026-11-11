// A sheet page drawn as a spreadsheet, which the app around it saves into
// its file. The format: skills/artifact/references/sheet.md.

import { headOf, isTotal, openBook } from "./book.js";
import { cellAt, draw, paint, readTab } from "./grid.js";
import { listen } from "./input.js";
import { boxName, boxOf, cellName } from "./text.js";

const tables = [...document.querySelectorAll("table[data-sheet]")];
const say = (message) =>
  parent.postMessage({ uagentDesign: 1, ...message }, "*");
const here = new URL(".", import.meta.url).href;

document.head.append(
  Object.assign(document.createElement("link"), {
    rel: "stylesheet",
    href: `${here}sheet.css`,
  }),
);

async function open() {
  const engine = await import(`${here}wasm.js`);
  await engine.default();
  // The engine can come ready inside a ResizeObserver's report (the page
  // helper's), where a page that resizes itself raises an error.
  await new Promise((done) => setTimeout(done));
  const book = openBook(engine, tables.map(readTab));

  let at = 0;
  // Per tab: its selection, whose anchor is the active cell, and the words
  // its columns are filtered by.
  const picks = tables.map(() => ({
    anchor: { row: 1, column: 1 },
    focus: { row: 1, column: 1 },
  }));
  const filters = tables.map(() => new Map());
  // The rows the shown tab's filters hide: no cell of them is selected.
  let hidden = new Set();
  const told = tables.map((_, sheet) => book.table(sheet));

  const tab = () => book.tabs[at];
  const size = () => ({
    rows: tab().rows.length,
    columns: tab().columns.length,
  });
  const within = ({ row, column }) => ({
    row: Math.max(1, Math.min(row, size().rows)),
    column: Math.max(1, Math.min(column, size().columns)),
  });
  // The nearest row in view, looking first the way the selection moves.
  const seen = (cell) => {
    const way = cell.row < picks[at].focus.row ? -1 : 1;
    for (const step of [way, -way]) {
      let { row } = cell;
      while (hidden.has(row)) row += step;
      if (row >= 1 && row <= size().rows) return { ...cell, row };
    }
    return cell;
  };
  const pick = () => ({
    anchor: within(picks[at].anchor),
    focus: within(picks[at].focus),
  });
  const box = () => boxOf(pick().anchor, pick().focus);
  const high = () => box().bottom - box().top + 1;
  const wide = () => box().right - box().left + 1;
  const hides = () => {
    const rows = new Set();
    for (const [column, words] of filters[at])
      tab().rows.forEach((row, r) => {
        if (
          r >= headOf(tab()) &&
          !isTotal(row) &&
          !book
            .shown(at, r + 1, column)
            .toLowerCase()
            .includes(words)
        )
          rows.add(r + 1);
      });
    return rows;
  };

  // Once a frame, however fast the selection moves.
  let saying = 0;
  const tell = () => {
    saying ||= requestAnimationFrame(() => {
      saying = 0;
      const { anchor } = pick();
      say({
        type: "cell",
        ref: `${tab().name}!${cellName(anchor)}`,
        raw: tab().rows[anchor.row - 1].cells[anchor.column - 1].text,
        range: `${tab().name}!${boxName(box())}`,
        summary: high() * wide() > 1 ? book.summary(at, box()) : undefined,
      });
    });
  };
  const sheet = {
    book,
    say,
    get at() {
      return at;
    },
    table: () => tables[at],
    tab,
    size,
    pick,
    box,
    high,
    wide,
    filterOf: (column) => filters[at].get(column) ?? "",

    // `from` true keeps the anchor; a cell given is the anchor; `still`
    // leaves the view where it is.
    select(focus, from, still) {
      const to = seen(within(focus));
      picks[at] = {
        anchor: from === true ? pick().anchor : seen(within(from || to)),
        focus: to,
      };
      paint(tables[at], box(), pick().anchor);
      if (!still)
        cellAt(tables[at], to)?.scrollIntoView({
          block: "nearest",
          inline: "nearest",
        });
      rest();
      tell();
    },

    // `changed` is what the book answered: true, false, or why not.
    did(changed) {
      if (typeof changed === "string")
        say({ type: "refused", message: changed });
      if (changed !== true) return false;
      // A filter is of a column by its place: with other columns it is off.
      book.tabs.forEach((tab, sheet) => {
        if (tab.columns.length !== widths[sheet]) filters[sheet].clear();
        widths[sheet] = tab.columns.length;
      });
      show(at);
      const errors = book.errors();
      book.tabs.forEach((_, tab) => {
        const table = book.table(tab);
        if (table === told[tab]) return;
        told[tab] = table;
        say({
          type: "edit",
          tab,
          tabs: tables.length,
          table,
          undo: book.canUndo(),
          redo: book.canRedo(),
          errors,
        });
      });
      return true;
    },
    run: (command) => sheet.did(commands[command]?.()),
    write: (text) => sheet.did(book.paste(at, pick().anchor, [[text]])),

    filter(column, words) {
      if (words.trim()) filters[at].set(column, words.trim().toLowerCase());
      else filters[at].delete(column);
      show(at);
    },
  };
  const widths = book.tabs.map((tab) => tab.columns.length);

  const commands = {
    undo: book.undo,
    redo: book.redo,
    // Only what is in view is cleared.
    clear: () =>
      book.paste(
        at,
        { row: box().top, column: box().left },
        Array.from({ length: high() }, (_, r) =>
          hidden.has(box().top + r) ? [] : Array(wide()).fill(""),
        ),
      ),
    "fill-down": () => book.fillDown(at, box()),
    "sort-up": () => book.sort(at, pick().anchor.column, false),
    "sort-down": () => book.sort(at, pick().anchor.column, true),
    "row-above": () => book.insertRows(at, box().top, high()),
    "row-below": () => book.insertRows(at, box().bottom + 1, high()),
    "row-delete": () =>
      [...hidden].some((row) => row >= box().top && row <= box().bottom)
        ? "Clear the filter to delete these rows."
        : book.deleteRows(at, box().top, high()),
    "column-left": () => book.insertColumns(at, box().left, wide()),
    "column-right": () => book.insertColumns(at, box().right + 1, wide()),
    "column-delete": () => book.deleteColumns(at, box().left, wide()),
  };

  const show = (index) => {
    if (!tables[index]) return;
    at = index;
    tables.forEach((table, tab) => {
      table.hidden = tab !== at;
    });
    hidden = hides();
    picks[at] = { anchor: seen(pick().anchor), focus: seen(pick().focus) };
    draw(tables[at], book, at, filters[at], hidden);
    paint(tables[at], box(), pick().anchor);
    rest();
    say({
      type: "sheet",
      tabs: book.tabs.map((tab) => tab.name),
      active: at,
      hidden: hidden.size,
    });
    tell();
  };
  const rest = listen(sheet);
  addEventListener("message", ({ source, data }) => {
    if (source !== parent || data?.uagentDesign !== 1) return;
    if (data.type === "tab") show(data.index);
    if (data.type === "do") sheet.run(data.command);
    if (data.type === "input") sheet.write(String(data.value));
  });
  show(0);
  return book.errors();
}

// Resolves to the cells in error, which the page helper reports.
export const ready = tables.length ? open() : Promise.resolve([]);
