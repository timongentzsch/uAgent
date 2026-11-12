// A sheet page drawn as a spreadsheet, which the app around it saves into
// its file. The format: skills/artifact/references/sheet.md.

import { headOf, isTotal, openBook } from "./book.js";
import { CHART } from "./chart.js";
import { listenCharts } from "./figure.js";
import { draw, figuresOf, paint, readTab, sizesOf } from "./grid.js";
import { listen } from "./input.js";
import { layoutOf } from "./layout.js";
import {
  boxName,
  boxNamed,
  boxOf,
  CELL,
  cellName,
  frozenOf,
  LAST,
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
  // Alone (its own tab, a print, the agent's browser) the page is its whole
  // table and nothing past it; in the pane it is a window on an endless grid.
  const whole = parent === window;
  // Cells drawn past the view on each side, so a scroll finds them there.
  const OVER = { rows: 16, columns: 4 };
  const area = document.body.appendChild(
    Object.assign(document.createElement("div"), { className: "sheet-area" }),
  );
  // How far each tab's grid has been scrolled: it runs a pane past that, the
  // table, the selection and the charts, up to the spreadsheet's last cell.
  const reached = tables.map(() => ({ rows: 0, columns: 0 }));
  const extent = () => {
    const { anchor, focus } = picks[at];
    const charts = tab().charts.map(
      ({ attrs }) => boxNamed(attrs["data-at"]) ?? { top: 0, left: 0 },
    );
    const far = (count, most, pane, ...needs) =>
      whole ? count : Math.min(most, Math.max(count, ...needs) + pane);
    return {
      rows: far(
        tab().rows.length,
        LAST.row,
        Math.ceil(innerHeight / sizes.row),
        reached[at].rows,
        anchor.row,
        focus.row,
        ...charts.map((chart) => chart.top),
      ),
      columns: far(
        tab().columns.length,
        LAST.column,
        Math.ceil(innerWidth / CELL.width),
        reached[at].columns,
        anchor.column,
        focus.column,
        ...charts.map((chart) => chart.left),
      ),
    };
  };
  // The shown tab's sizes (measured at each change), where its cells lie,
  // what it holds in view, and the rows and columns last drawn.
  let sizes = { widths: [], heights: [], row: CELL.height };
  let layout = layoutOf([], [], 0, CELL.height);
  let frozen = { rows: 0, columns: 0 };
  let grid = { rows: 0, columns: 0 };
  let drawn = { top: 1, bottom: 0, left: 1, right: 0 };
  // The row numbers' column is as wide as the last row's number needs.
  const fit = () => {
    grid = extent();
    layout = layoutOf(
      sizes.widths,
      sizes.heights.map((height, r) => (hidden.has(r + 1) ? 0 : height)),
      Math.max(40, 14 + 7 * `${grid.rows}`.length),
      sizes.row,
    );
    Object.assign(area.style, {
      width: `${layout.x(grid.columns + 1)}px`,
      height: `${layout.y(grid.rows + 1)}px`,
    });
  };
  // What is in view past the held rows and columns, as cells.
  const inView = () => ({
    top: layout.rowAt(scrollY + layout.y(frozen.rows + 1)),
    bottom: layout.rowAt(scrollY + innerHeight),
    left: layout.columnAt(scrollX + layout.x(frozen.columns + 1)),
    right: layout.columnAt(scrollX + innerWidth),
  });
  const render = () => {
    const view = inView();
    const span = (from, to, held, most) =>
      Array.from(
        {
          length: Math.max(
            0,
            Math.min(to, most) - Math.max(from, held + 1) + 1,
          ),
        },
        (_, n) => Math.max(from, held + 1) + n,
      );
    drawn = whole
      ? { top: 1, bottom: grid.rows, left: 1, right: grid.columns }
      : {
          top: view.top - OVER.rows,
          bottom: view.bottom + OVER.rows,
          left: view.left - OVER.columns,
          right: view.right + OVER.columns,
        };
    const held = (count) => Array.from({ length: count }, (_, n) => n + 1);
    draw(tables[at], book, at, filters[at], {
      layout,
      frozen,
      rows: [
        ...held(frozen.rows),
        ...span(drawn.top, drawn.bottom, frozen.rows, grid.rows),
      ].filter((row) => !hidden.has(row)),
      columns: [
        ...held(frozen.columns),
        ...span(drawn.left, drawn.right, frozen.columns, grid.columns),
      ],
    });
    tables[at].classList.toggle("sheet-window", !whole);
    paint(tables[at], box(), pick().anchor);
  };
  // Drawn again once the view leaves what is drawn.
  const follow = () => {
    const view = inView();
    if (
      view.top < Math.max(drawn.top, frozen.rows + 1) ||
      view.left < Math.max(drawn.left, frozen.columns + 1) ||
      Math.min(view.bottom, grid.rows) > drawn.bottom ||
      Math.min(view.right, grid.columns) > drawn.right
    )
      render();
  };
  // A cell is brought into view past what is held; the grid reaches it first.
  const reveal = (cell) => {
    fit();
    const { left, top, width, height } = layout.rect(cell);
    const [x, y] = [layout.x(frozen.columns + 1), layout.y(frozen.rows + 1)];
    const to = (at, size, held, scrolled, room, still) =>
      still || (at >= scrolled + held && at + size <= scrolled + room)
        ? scrolled
        : at < scrolled + held
          ? at - held
          : at + size - room;
    scrollTo(
      to(left, width, x, scrollX, innerWidth, cell.column <= frozen.columns),
      to(top, height, y, scrollY, innerHeight, cell.row <= frozen.rows),
    );
    follow();
  };
  const within = ({ row, column }) => ({
    row: Math.max(1, Math.min(row, whole ? grid.rows : LAST.row)),
    column: Math.max(1, Math.min(column, whole ? grid.columns : LAST.column)),
  });
  // The nearest row in view, looking first the way the selection moves.
  const seen = (cell) => {
    const way = cell.row < picks[at].focus.row ? -1 : 1;
    for (const step of [way, -way]) {
      let { row } = cell;
      while (hidden.has(row)) row += step;
      if (row >= 1 && row <= LAST.row) return { ...cell, row };
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
    // Where a cell lies in the page, drawn or not.
    rect: (cell) => layout.rect(cell),
    layout: () => layout,
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
      if (!still) reveal(to);
      paint(tables[at], box(), pick().anchor);
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
    picks[at] = { anchor: seen(pick().anchor), focus: seen(pick().focus) };
    sizes = sizesOf(tables[at], book, at);
    const held = frozenOf(tab().attrs["data-frozen"], headOf(tab()));
    frozen = {
      rows: Math.min(held.rows, tab().rows.length),
      columns: Math.min(held.columns, tab().columns.length),
    };
    fit();
    render();
    charts();
    rest();
    say({
      type: "sheet",
      tabs: offered.map((tab) => book.tabs[tab].name),
      hidden: hidden.size,
    });
    tell();
  };
  // A scroll draws the window it leaves the last one for, once a frame, and
  // at the grid's end makes it a pane longer. The editor is no part of the
  // table: what is being typed stays through it.
  const root = document.documentElement;
  let scrolled = 0;
  addEventListener(
    "scroll",
    () => {
      scrolled ||= requestAnimationFrame(() => {
        scrolled = 0;
        const ended = (at, room, all, far) =>
          at + room >= all - sizes.row ? far : 0;
        const rows = ended(scrollY, innerHeight, root.scrollHeight, grid.rows);
        const columns = ended(
          scrollX,
          innerWidth,
          root.scrollWidth,
          grid.columns,
        );
        if (!whole && (rows || columns)) {
          reached[at] = {
            rows: Math.max(reached[at].rows, rows),
            columns: Math.max(reached[at].columns, columns),
          };
          fit();
        }
        follow();
      });
    },
    { passive: true },
  );
  addEventListener("resize", () => {
    fit();
    follow();
  });
  // Another look from the app (its theme, its zoom) is other sizes.
  let restyled = 0;
  new MutationObserver(() => {
    restyled ||= requestAnimationFrame(() => {
      restyled = 0;
      show(at);
    });
  }).observe(root, { attributes: true, attributeFilter: ["style"] });
  const { rest, hold } = listen(sheet);
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
