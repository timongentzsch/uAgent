// A sheet page drawn as a spreadsheet, which the app around it saves into
// its file. The format: skills/artifact/references/sheet.md.

import { headOf, isTotal, openBook } from "./book.js";
import { CHART } from "./chart.js";
import { listenCharts } from "./figure.js";
import { cellAt, draw, figuresOf, paint, readTab } from "./grid.js";
import { listen } from "./input.js";
import {
  boxName,
  boxNamed,
  boxOf,
  CELL,
  cellName,
  SPARE,
  sheetText,
  toCsv,
} from "./text.js";
import { csvTab, workbookTabs } from "./workbook.js";

const tables = [...document.querySelectorAll("table[data-sheet]")];
// The page of an Excel or CSV file holds no tables: it names the file,
// percent-encoded, which lies beside it.
const named = document.querySelector('meta[name="sheet"]')?.content;
const file = tables.length || named === "1" ? "" : (named ?? "");
const title = decodeURIComponent(file).replace(/\.[^.]+$/, "");
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
  const read = async () => {
    const answer = await fetch(`./${file}`);
    if (!answer.ok) throw new Error(`the host answered ${answer.status}`);
    return /\.csv$/i.test(file)
      ? [csvTab(engine, title, await answer.text())]
      : workbookTabs(engine, new Uint8Array(await answer.arrayBuffer()), title);
  };
  // A file that cannot be read is said in its page, which then draws nothing.
  const unread = (error) => {
    const note = document.body.appendChild(document.createElement("p"));
    note.className = "sheet-notice";
    note.textContent = `${decodeURIComponent(file)} cannot be shown: ${error.message ?? error}`;
    return note.textContent;
  };
  const written = file ? await read().catch(unread) : tables.map(readTab);
  if (typeof written === "string") return [written];
  const book = openBook(engine, written, !!file);
  // The tabs to choose from: a workbook's hidden sheets only calculate.
  const offered = written.flatMap((tab, at) => (tab.hidden ? [] : [at]));
  for (const figure of tables.flatMap(figuresOf)) figure.remove();
  // What is left follows no table: it is not drawn, and stays in the file.
  const stray = [...document.querySelectorAll("figure[data-chart]")].map(
    (figure) =>
      `chart ${figure.dataset.range ?? ""}: a chart must directly follow its table`,
  );
  const errors = () => [...stray, ...book.errors()];
  if (file)
    for (const { name } of book.tabs) {
      const table = document.body.appendChild(document.createElement("table"));
      table.dataset.sheet = name;
      tables.push(table);
    }

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
  // The grid runs on past the table: as far as the selection, the scroll and
  // the tab's charts need and a pane beyond, at most SPARE past its end.
  // Worked out at each draw, so it is no larger than what is in use.
  let grid = { rows: 0, columns: 0 };
  const size = () => grid;
  const extent = () => {
    const { anchor, focus } = picks[at];
    const charts = tab().charts.map(
      ({ attrs }) => boxNamed(attrs["data-at"]) ?? { top: 0, left: 0 },
    );
    const heads = [...(tables[at].tHead?.rows[0].cells ?? [])];
    const far = (count, most, pane, ...needs) =>
      Math.min(count + most, Math.max(count, ...needs) + pane);
    return {
      rows: far(
        tab().rows.length,
        SPARE.rows,
        Math.ceil(innerHeight / CELL.height),
        anchor.row,
        focus.row,
        Math.floor((scrollY + innerHeight) / CELL.height),
        ...charts.map((chart) => chart.top),
      ),
      columns: far(
        tab().columns.length,
        SPARE.columns,
        Math.ceil(innerWidth / CELL.width),
        anchor.column,
        focus.column,
        heads.findLastIndex((head) => head.offsetLeft < scrollX + innerWidth),
        ...charts.map((chart) => chart.left),
      ),
    };
  };
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
        raw: tab().rows[anchor.row - 1]?.cells[anchor.column - 1]?.text ?? "",
        range: `${tab().name}!${boxName(box())}`,
        summary: high() * wide() > 1 ? book.summary(at, box()) : undefined,
      });
    });
  };
  const sheet = {
    book,
    say,
    locked: !!file,
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
      // At its last drawn cells the grid has more to show.
      if (!still && (to.row >= grid.rows || to.column >= grid.columns)) grow();
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
      // A filter is of a column of the table.
      book.tabs.forEach((tab, sheet) => {
        for (const column of filters[sheet].keys())
          if (column > tab.columns.length) filters[sheet].delete(column);
      });
      show(at);
      const found = errors();
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
          errors: found,
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
  // A filter is of a column by its place: with a column put in or taken
  // out before it, it would be another column's.
  const shifted = (changed) => {
    if (changed === true) filters[at].clear();
    return changed;
  };

  const out = (as, data) => say({ type: "file", as, data });
  const commands = {
    undo: book.undo,
    redo: book.redo,
    // The open tab's cells go out as they show.
    xlsx: () => out("xlsx", book.xlsx()),
    // The mark before it tells Excel the text is UTF-8.
    csv: () =>
      out(
        "csv",
        "\uFEFF" +
          toCsv(
            tab().rows.map((row, r) =>
              row.cells.map((_, c) => book.shown(at, r + 1, c + 1)),
            ),
          ),
      ),
    sheet: () => out("sheet", sheetText(title, book.tabs)),
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
    chart: () =>
      high() > 1 && wide() > 1
        ? book.setCharts(at, (charts) => [
            ...charts,
            {
              attrs: {
                "data-chart": CHART["data-chart"],
                "data-range": boxName(box()),
                "data-at": cellName({
                  row: box().top,
                  column: box().right + 2,
                }),
                "data-size": CHART["data-size"],
              },
              caption: "",
            },
          ])
        : "Select the cells to chart, with their labels.",
    "sort-up": () => book.sort(at, pick().anchor.column, false),
    "sort-down": () => book.sort(at, pick().anchor.column, true),
    "row-above": () => book.insertRows(at, box().top, high()),
    "row-below": () => book.insertRows(at, box().bottom + 1, high()),
    "row-delete": () =>
      [...hidden].some((row) => row >= box().top && row <= box().bottom)
        ? "Clear the filter to delete these rows."
        : book.deleteRows(at, box().top, high()),
    "column-left": () => shifted(book.insertColumns(at, box().left, wide())),
    "column-right": () =>
      shifted(book.insertColumns(at, box().right + 1, wide())),
    "column-delete": () => shifted(book.deleteColumns(at, box().left, wide())),
  };

  const show = (index) => {
    if (!tables[index]) return;
    at = index;
    tables.forEach((table, tab) => {
      table.hidden = tab !== at;
    });
    hidden = hides();
    grid = extent();
    picks[at] = { anchor: seen(pick().anchor), focus: seen(pick().focus) };
    draw(tables[at], book, at, filters[at], hidden, grid);
    charts();
    paint(tables[at], box(), pick().anchor);
    rest();
    say({
      type: "sheet",
      tabs: offered.map((tab) => book.tabs[tab].name),
      hidden: hidden.size,
    });
    tell();
  };
  // Where more is needed than is drawn, the grid is drawn again; not under
  // a cell being typed in, whose text a new grid would drop.
  const grow = () => {
    const next = extent();
    if (!typing() && (next.rows > grid.rows || next.columns > grid.columns))
      show(at);
  };
  const root = document.documentElement;
  addEventListener(
    "scroll",
    () => {
      if (
        scrollY + innerHeight >= root.scrollHeight - CELL.height ||
        scrollX + innerWidth >= root.scrollWidth - CELL.width
      )
        grow();
    },
    { passive: true },
  );
  addEventListener("resize", grow);
  const { rest, hold, typing } = listen(sheet);
  sheet.hold = hold;
  const charts = listenCharts(sheet);
  addEventListener("message", ({ source, data }) => {
    if (source !== parent || data?.uagentDesign !== 1) return;
    if (data.type === "do") sheet.run(data.command);
    if (data.type === "input") sheet.write(String(data.value));
  });
  // The address says which tab shows: "#2" is the second.
  const said = () =>
    show(offered[(Number(location.hash.slice(1)) || 1) - 1] ?? offered[0]);
  addEventListener("hashchange", said);
  said();
  return errors();
}

// Resolves to the cells in error, which the page helper reports.
export const ready = tables.length || file ? open() : Promise.resolve([]);
