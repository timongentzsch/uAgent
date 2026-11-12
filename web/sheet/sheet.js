// A sheet page drawn as a spreadsheet, which the app around it saves into
// its file. The format: skills/artifact/references/sheet.md.

import { headOf, isTotal, openBook } from "./book.js";
import { CHART } from "./chart.js";
import { listenCharts } from "./figure.js";
import { draw, figuresOf, paint, readTab, sizesOf } from "./grid.js";
import { listen } from "./input.js";
import { layoutOf } from "./layout.js";
import { suggestions } from "./suggest.js";
import {
  boxName,
  boxNamed,
  boxOf,
  CELL,
  cellName,
  frozenOf,
  LAST,
  letters,
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

// The grid is measured as its stylesheet lays it out: drawn once that has
// loaded, or failed to.
const styled = new Promise((done) =>
  document.head.append(
    Object.assign(document.createElement("link"), {
      rel: "stylesheet",
      href: `${here}sheet.css`,
      onload: done,
      onerror: done,
    }),
  ),
);

async function open() {
  const engine = await import(`${here}wasm.js`);
  await engine.default();
  await styled;
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
  // A file that cannot be read or calculated is said in its page, which
  // then draws nothing: a sheet's tables stay as they are written.
  const unread = (error) => {
    const note = document.createElement("p");
    note.className = "sheet-notice";
    note.textContent = `${file ? `${decodeURIComponent(file)} cannot be shown` : "This sheet is shown as written"}: ${
      error instanceof WebAssembly.RuntimeError
        ? "the engine cannot calculate it"
        : (error.message ?? error)
    }`;
    document.body.prepend(note);
    return note.textContent;
  };
  const written = file ? await read().catch(unread) : tables.map(readTab);
  if (typeof written === "string") return [written];
  const book = openBook(engine, written, !!file);
  if (!book) return [unread("the engine cannot calculate it")];
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
  const root = document.documentElement;
  // Cells drawn past the view on each side, so a scroll finds them there.
  const OVER = { rows: 16, columns: 4 };
  const area = document.body.appendChild(
    Object.assign(document.createElement("div"), {
      className: whole ? "sheet-area" : "sheet-area sheet-stretch",
    }),
  );
  // The scroll area stands for a stretch of the grid's rows, far shorter
  // than a browser can scroll (2^24 px in some); `base` is where in the
  // grid it starts, `tall` how far it runs. A test makes it shorter, never
  // shorter than a few panes.
  const SPAN = 4_000_000;
  const long = () =>
    whole
      ? Infinity
      : Math.max(
          3 * innerHeight,
          Math.min(
            SPAN,
            Number(root.style.getPropertyValue("--sheet-span")) || SPAN,
          ),
        );
  let [base, tall] = [0, 0];
  // How far each tab's grid has been scrolled: it runs a pane past that, the
  // table, the selection and the charts, up to the spreadsheet's last cell.
  const reached = tables.map(() => ({ rows: 0, columns: 0 }));
  // Each tab's rows as high as they were drawn, while it has as many: a
  // wrapped row is as high as the browser made it, which sizes only reckon.
  const kept = tables.map(() => ({ rows: -1, heights: new Map() }));
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
  // Where the stretch began when the grid was last drawn.
  let based = 0;
  let tinted;
  // Where the page itself last moved the view to: a scroll that ends there
  // is not a person's, and makes the grid no longer.
  let led = {};
  // The view ends where the grid does; a browser may leave it past the end
  // of a grid made shorter.
  const settle = (x = scrollX, y = scrollY) => {
    const within = (at, end, room) =>
      whole ? at : Math.max(0, Math.min(at, end - room));
    const to = [
      within(x, layout.x(grid.columns + 1), innerWidth),
      within(y, tall, innerHeight),
    ];
    if (to[0] === scrollX && to[1] === scrollY) return;
    scrollTo(...to);
    led = { x: scrollX, y: scrollY };
  };
  // The stretch holds `view`, a place in the grid to have at the view's
  // top. Asked to `centre`, with that in an outer quarter of it, the
  // stretch moves so that it is in the middle, and the scroll with it, so
  // the view stays on its rows: where the page itself moves the view, and
  // where a person's scroll has come to rest, never under one going on.
  const stretch = (view = scrollY + base, x = scrollX, centre = false) => {
    const all = layout.y(grid.rows + 1);
    const [was, at, span] = [base, view - base, long()];
    if (centre && (at < span / 4 || at > (span * 3) / 4))
      base = Math.round(view - span / 2);
    base = Math.max(0, Math.min(base, all - span));
    if (base !== was) drawn = { top: 1, bottom: 0, left: 1, right: 0 };
    tall = Math.min(span, all - base);
    area.style.height = `${tall}px`;
    settle(x, view - base);
  };
  // The row numbers' column is as wide as the last row's number needs:
  // of another width, it moves every column, and nothing drawn stands.
  const fit = () => {
    grid = extent();
    const edge = Math.max(40, 14 + 7 * `${grid.rows}`.length);
    if (edge !== layout.x(1)) drawn = { top: 1, bottom: 0, left: 1, right: 0 };
    layout = layoutOf(
      sizes.widths,
      sizes.heights.map((height, r) => (hidden.has(r + 1) ? 0 : height)),
      edge,
      sizes.row,
    );
    area.style.width = `${layout.x(grid.columns + 1)}px`;
    stretch();
  };
  // What is in view past the held rows and columns, as cells.
  const inView = () => ({
    top: layout.rowAt(scrollY + base + layout.y(frozen.rows + 1)),
    bottom: layout.rowAt(scrollY + base + innerHeight),
    left: layout.columnAt(scrollX + layout.x(frozen.columns + 1)),
    right: layout.columnAt(scrollX + innerWidth),
  });
  // The rows drawn are as high as the browser made them: true where one
  // was not as high as reckoned. The rows in view stay where they were.
  const measure = () => {
    const { top } = inView();
    let [changed, above] = [false, 0];
    for (const line of tables[at].tBodies[0].rows) {
      const row = Number(line.cells[0].dataset.row);
      const { height } = line.getBoundingClientRect();
      const was = sizes.heights[row - 1];
      if (was === undefined || height === was) continue;
      if (row > frozen.rows && row < top) above += height - was;
      sizes.heights[row - 1] = height;
      kept[at].heights.set(row, height);
      changed = true;
    }
    if (changed) fit();
    if (above) settle(scrollX, scrollY + above);
    return changed;
  };
  // Drawn again where a row's real height moved those after it, and what
  // lies over the grid put there.
  const render = () => {
    let moved = base !== based;
    lay();
    for (let pass = 0; pass < 4 && measure(); pass++) {
      moved = true;
      lay();
    }
    tables[at].classList.toggle("sheet-window", !whole);
    paint(tables[at], box(), pick().anchor, tinted);
    based = base;
    if (moved) {
      charts();
      place();
    }
  };
  const lay = () => {
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
    // No further than the grid runs: grown, it is drawn again from there.
    drawn = whole
      ? { top: 1, bottom: grid.rows, left: 1, right: grid.columns }
      : {
          top: view.top - OVER.rows,
          bottom: Math.min(view.bottom + OVER.rows, grid.rows),
          left: view.left - OVER.columns,
          right: Math.min(view.right + OVER.columns, grid.columns),
        };
    const held = (count) => Array.from({ length: count }, (_, n) => n + 1);
    draw(tables[at], book, at, filters[at], {
      layout,
      base,
      frozen,
      row: sizes.row,
      rows: [
        ...held(frozen.rows),
        ...span(drawn.top, drawn.bottom, frozen.rows, grid.rows),
      ].filter((row) => !hidden.has(row)),
      columns: [
        ...held(frozen.columns),
        ...span(drawn.left, drawn.right, frozen.columns, grid.columns),
      ],
    });
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
    stretch(
      to(top, height, y, scrollY + base, innerHeight, cell.row <= frozen.rows),
      to(left, width, x, scrollX, innerWidth, cell.column <= frozen.columns),
      true,
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
  // A filter's words are in what a cell shows; after "=" they are all of
  // it, and after "<>" a cell that shows just them is left out.
  const matches = (shown, words) =>
    words.startsWith("=")
      ? shown === words.slice(1)
      : words.startsWith("<>")
        ? shown !== words.slice(2)
        : shown.includes(words);
  const hides = () => {
    const rows = new Set();
    for (const [column, words] of filters[at])
      tab().rows.forEach((row, r) => {
        if (
          r >= headOf(tab()) &&
          !isTotal(row) &&
          !matches(book.shown(at, r + 1, column).toLowerCase(), words)
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
        marks: book.state(at, anchor.row, anchor.column),
        rules: book.rules(at, anchor.column),
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
    // The cells a picked chart reads, tinted; none without a box.
    reads(box) {
      tinted = box;
      paint(tables[at], sheet.box(), pick().anchor, tinted);
    },
    // The rows and columns the tab holds in view.
    held: () => frozen,
    // Where a cell lies in the page: where it is drawn (a held one does not
    // scroll), or where it would be, as far as the stretch runs.
    rect(cell) {
      const drawn = tables[at].querySelector(
        `tbody [data-row="${cell.row}"][data-column="${cell.column}"]`,
      );
      const box = drawn?.getBoundingClientRect() ?? layout.rect(cell);
      const { left, top, width, height } = box;
      return drawn
        ? { left: left + scrollX, top: top + scrollY, width, height }
        : { ...box, top: Math.max(0, Math.min(top - base, tall - height)) };
    },
    // Brings the active cell into view.
    see: () => reveal(pick().anchor),
    offers: (what) => offers.has(what),
    // The view at the row's start, or the grid's: a held cell is in view
    // wherever the grid is scrolled, and would lead nowhere.
    start(grid) {
      stretch(grid ? 0 : undefined, 0, true);
      follow();
    },
    // What lies over the grid is laid in it, and by its places in the page.
    area,
    layout: () => ({
      ...layout,
      y: (row) => layout.y(row) - base,
      rowAt: (place) => layout.rowAt(place + base),
    }),
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
      paint(tables[at], box(), pick().anchor, tinted);
      rest();
      tell();
    },

    // `changed` is what the book answered: true, false, or why not.
    did(changed) {
      if (typeof changed === "string")
        say({ type: "refused", message: changed });
      if (changed !== true) return false;
      // Tabs put in or taken out (by Undo too) are every table written anew
      // over those the file has, and a table here for each.
      if (book.tabs.length !== tables.length) {
        say({
          type: "edit",
          tab: 0,
          tabs: told.length,
          all: true,
          table: book.tabs.map((_, tab) => book.table(tab)).join("\n"),
          undo: book.canUndo(),
          redo: book.canRedo(),
          errors: errors(),
        });
        retab();
        show(Math.min(at, tables.length - 1));
        return true;
      }
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
    run: (command, value) => sheet.did(commands[command]?.(value)),
    // The shown tab's table as text typed in the active cell would make it.
    draft() {
      const cut = book.draft(at, pick().anchor);
      return cut && { tab: at, tabs: tables.length, ...cut };
    },
    write: (text) => sheet.did(book.paste(at, pick().anchor, [[text]])),

    filter(column, words) {
      if (words.trim()) filters[at].set(column, words.trim().toLowerCase());
      else filters[at].delete(column);
      show(at);
    },
  };
  const offers = new Set();
  const carried = (rows, columns) => {
    if (
      rows &&
      [...hidden].some((row) => row >= box().top && row <= box().bottom)
    )
      return "Clear the filter to move these rows.";
    const changed = rows
      ? book.moveRows(at, box().top, high(), rows)
      : shifted(book.moveColumns(at, box().left, wide(), columns));
    const on = ({ row, column }) => ({
      row: row + rows,
      column: column + columns,
    });
    if (changed === true)
      picks[at] = { anchor: on(picks[at].anchor), focus: on(picks[at].focus) };
    return changed;
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
    // The selected rows or columns among their neighbours, selected still.
    "row-move-up": () => carried(-1, 0),
    "row-move-down": () => carried(1, 0),
    "column-move-left": () => carried(0, -1),
    "column-move-right": () => carried(0, 1),
    "column-fit": () => book.widen(at, box(), undefined),
    "clear-looks": () => book.unstyle(at, box(), hidden),
    // What the app around the page offers to do when asked: a comment.
    offers: (what) => void offers.add(String(what)),
    // Held up to the active cell: its row, or its column, and those before.
    "freeze-rows": () => freeze(pick().anchor.row, frozen.columns),
    "freeze-columns": () => freeze(frozen.rows, pick().anchor.column),
    unfreeze: () => freeze(0, 0),
    "tab-add": () => book.addTab(),
    "tab-copy": () => book.copyTab(at),
    "tab-left": () => book.moveTab(at, -1),
    "tab-right": () => book.moveTab(at, 1),
    "tab-delete": () => book.dropTab(at),
    "tab-name": (name) => book.nameTab(at, String(name ?? "")),
    // One rule of the active cell's column as the app's dialog set it, by
    // its place among them and what it read there; without a rule, that one
    // is taken out. The cell is said again either way: the dialog waits.
    rule(value) {
      const { of, place, text, rule } = JSON.parse(value);
      const { column } = pick().anchor;
      tell();
      return of === `${tab().name}!${letters(column)}`
        ? book.setRule(at, column, place, text, rule)
        : "Not changed: another column is selected than the rule was set for.";
    },
    // What is being typed in the cell, from the app's bar too: written, or
    // given up.
    commit: () => void commit(),
    cancel: () => void rest(),
    // A look for the selection's cells in view; a word (Bold) is its own
    // value.
    ...Object.fromEntries(
      ["bold", "italic", "wrap", "align", "fill", "ink", "format"].map(
        (name) => [
          name,
          (value = name) => book.style(at, box(), name, String(value), hidden),
        ],
      ),
    ),
  };
  // What is kept of each tab (its place, its filters) is let go with them.
  const retab = () => {
    for (const table of tables) table.remove();
    const all = (make) => book.tabs.map((_, tab) => make(tab));
    tables.splice(
      0,
      Infinity,
      ...all(() => document.body.appendChild(document.createElement("table"))),
    );
    picks.splice(
      0,
      Infinity,
      ...all(() => ({
        anchor: { row: 1, column: 1 },
        focus: { row: 1, column: 1 },
      })),
    );
    filters.splice(0, Infinity, ...all(() => new Map()));
    reached.splice(0, Infinity, ...all(() => ({ rows: 0, columns: 0 })));
    kept.splice(0, Infinity, ...all(() => ({ rows: -1, heights: new Map() })));
    told.splice(0, Infinity, ...all((tab) => book.table(tab)));
    offered.splice(0, Infinity, ...all((tab) => tab));
  };
  const freeze = (rows, columns) =>
    book.mark(
      at,
      "data-frozen",
      `${rows}${columns ? ` ${letters(columns)}` : ""}`,
    );

  // `still` leaves a cell being typed in as it is.
  const show = (index, still) => {
    if (!tables[index]) return;
    at = index;
    tables.forEach((table, tab) => {
      table.hidden = tab !== at;
      table.dataset.sheet = book.tabs[tab].name;
    });
    hidden = hides();
    picks[at] = { anchor: seen(pick().anchor), focus: seen(pick().focus) };
    sizes = sizesOf(tables[at], book, at);
    if (kept[at].rows !== tab().rows.length)
      kept[at] = { rows: tab().rows.length, heights: new Map() };
    for (const [row, height] of kept[at].heights)
      sizes.heights[row - 1] = height;
    const held = frozenOf(tab().attrs["data-frozen"], headOf(tab()));
    frozen = {
      rows: Math.min(held.rows, tab().rows.length),
      columns: Math.min(held.columns, tab().columns.length),
    };
    fit();
    render();
    charts();
    if (still) place();
    else rest();
    say({
      type: "sheet",
      tabs: offered.map((tab) => book.tabs[tab].name),
      hidden: hidden.size,
      // How many cells of other tabs read the one shown.
      readers: book.readers(at),
    });
    tell();
  };
  // A scroll draws the window it leaves the last one for, once a frame, and
  // at the grid's end makes it a pane longer. The editor is no part of the
  // table: what is being typed stays through it.
  let scrolled = 0;
  // Where a scroll rests, the stretch may move under the view: one driven
  // by a wheel, a key or a finger lifted, shortly before it began or since.
  // A scrollbar's thumb says none of these, and held still it is no rest:
  // the stretch would move under it.
  let [resting, began, driven] = [0, 0, -Infinity];
  for (const type of ["wheel", "keydown", "touchend"])
    addEventListener(type, () => (driven = performance.now()), {
      passive: true,
    });
  const rested = () => {
    resting = 0;
    if (driven < began - 300) return;
    stretch(undefined, undefined, true);
    follow();
  };
  addEventListener(
    "scroll",
    () => {
      if (!resting) began = performance.now();
      clearTimeout(resting);
      if (!whole) resting = setTimeout(rested, 150);
      scrolled ||= requestAnimationFrame(() => {
        scrolled = 0;
        // The grid grows where the stretch ends with it.
        const ended = (at, room, all, far) =>
          at + room >= all - sizes.row ? far : 0;
        const rows =
          base + tall >= layout.y(grid.rows + 1) &&
          ended(scrollY, innerHeight, root.scrollHeight, grid.rows);
        const columns = ended(
          scrollX,
          innerWidth,
          root.scrollWidth,
          grid.columns,
        );
        const own = scrollX === led.x && scrollY === led.y;
        if (!whole && !own && (rows || columns)) {
          reached[at] = {
            rows: Math.max(reached[at].rows, rows),
            columns: Math.max(reached[at].columns, columns),
          };
          fit();
        }
        follow();
        place();
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
      // The view keeps its rows, which lie elsewhere at other sizes.
      const { top } = inView();
      for (const { heights } of kept) heights.clear();
      show(at, true);
      stretch(layout.y(top) - layout.y(frozen.rows + 1), scrollX, true);
      follow();
    });
  }).observe(root, { attributes: true, attributeFilter: ["style"] });
  const { rest, hold, place, type, commit } = listen(sheet);
  sheet.hold = hold;
  const charts = listenCharts(sheet);
  addEventListener("message", ({ source, data }) => {
    if (source !== parent || data?.uagentDesign !== 1) return;
    if (data.type === "do") sheet.run(data.command, data.value);
    if (data.type === "input") type(String(data.value));
    // What the app's formula bar may take after `text`.
    if (data.type === "suggest") {
      const text = String(data.text);
      say({ type: "suggest", text, ...suggestions(text, book.tokens) });
    }
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
