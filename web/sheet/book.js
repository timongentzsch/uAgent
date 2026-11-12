// A sheet's tabs as its file keeps them. What was typed is the record: the
// engine reads "4.5%" as 0.045, so it only calculates and moves formulas.

import { CHARTS } from "./chart.js";
import { FILLS, readRules, ruleInput, slidRules } from "./rules.js";
import {
  boxName,
  boxNamed,
  cellName,
  frozenOf,
  inOrder,
  letters,
  moved,
  slid,
  slidBox,
  tableText,
} from "./text.js";

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

// The engine lets the last rule added that matches a cell give its fill:
// the table's first, so a column's lie over them.
const rulesOf = (tab) =>
  [
    readRules(tab.attrs["data-rule"], tab.columns.length),
    ...tab.columns.map((column, c) =>
      readRules(column["data-rule"], tab.columns.length, c + 1),
    ),
  ].reduce((all, { rules, unread }) => ({
    rules: [...all.rules, ...rules],
    unread: [...all.unread, ...unread],
  }));
const FILLED = Object.fromEntries(
  Object.entries(FILLS).map(([name, color]) => [color, name]),
);
// A cell's looks a person sets. One is a word of its class, which stands
// in place of the others of its kind; or what an attribute may say.
const WORDS = {
  bold: ["bold"],
  italic: ["italic"],
  wrap: ["wrap"],
  align: ["left", "center", "right"],
};
// The text colours, as a workbook sent out has them.
const INKS = {
  good: "#116329",
  warn: "#855600",
  bad: "#B31D28",
  muted: "#555555",
};
const SAID = { fill: Object.keys(FILLS), ink: Object.keys(INKS) };
// Where the engine keeps each look, and what it calls a colour.
const STYLED = {
  bold: "font.b",
  italic: "font.i",
  wrap: "alignment.wrap_text",
  align: "alignment.horizontal",
  fill: "fill.color",
  ink: "font.color",
};
const COLOURED = { fill: FILLS, ink: INKS };
const has = (attrs, word) => !!attrs.class?.split(/\s+/).includes(word);

// A chart goes with its rows or columns when its whole range does.
const slidCharts = (charts, across, at, count) =>
  charts.flatMap((chart) => {
    const range = boxNamed(chart.attrs["data-range"]);
    const on = boxNamed(chart.attrs["data-at"]);
    const left = range && slidBox(range, across, at, count);
    if (range && !left) return [];
    const place = on && { row: on.top, column: on.left };
    const way = across ? "column" : "row";
    if (place) place[way] = slid(place[way], at, count) ?? at;
    return [
      {
        ...chart,
        attrs: {
          ...chart.attrs,
          ...(left && { "data-range": boxName(left) }),
          ...(place && { "data-at": cellName(place) }),
        },
      },
    ];
  });

// A table's rules name columns by letter: they follow their columns.
const slidAttrs = ({ attrs, columns }, at, count) =>
  attrs["data-rule"]
    ? {
        ...attrs,
        "data-rule": slidRules(attrs["data-rule"], columns.length, (column) =>
          slid(column, at, count),
        ),
      }
    : attrs;

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

// The grid runs on past a tab's table. Written to there, the table reaches
// that far: blank cells under and beside it, a header row's headers too.
const reaching = (tab, rows, columns) => {
  const wider = Math.max(0, columns - tab.columns.length);
  if (!wider && rows <= tab.rows.length) return tab;
  const more = (row) =>
    wider
      ? {
          ...row,
          cells: [
            ...row.cells,
            ...Array.from({ length: wider }, () => blank(isHeader(row))),
          ],
        }
      : row;
  const width = tab.columns.length + wider;
  return {
    ...tab,
    columns: [...tab.columns, ...Array.from({ length: wider }, () => ({}))],
    rows: [
      ...tab.rows.map(more),
      ...Array.from({ length: rows - tab.rows.length }, () => ({
        attrs: {},
        cells: Array.from({ length: width }, () => blank()),
      })),
    ],
  };
};
// The file holds what is used: no blank rows or columns after it.
const bare = (attrs) => !Object.keys(attrs).length;
const empty = (cell) => !cell.text && bare(cell.attrs);
const trimmed = (tab) => {
  const rows =
    tab.rows.findLastIndex(
      (row) => !bare(row.attrs) || !row.cells.every(empty),
    ) + 1;
  const columns =
    tab.columns.findLastIndex(
      (column, c) =>
        !bare(column) || !tab.rows.every((row) => empty(row.cells[c])),
    ) + 1;
  return rows === tab.rows.length && columns === tab.columns.length
    ? tab
    : {
        ...tab,
        columns: tab.columns.slice(0, columns),
        rows: tab.rows
          .slice(0, rows)
          .map((row) => ({ ...row, cells: row.cells.slice(0, columns) })),
      };
};

const LOCKED = "This file is read-only: save it as a sheet to change it.";
// The host keeps no file over 1,024 KB: the tabs' tables may come to this.
const TABLES = 1_000_000;
const bytes = (text) => new TextEncoder().encode(text).length;

// `locked` for the tabs of an Excel or CSV file: no change is made to them.
export function openBook(engine, written, locked) {
  let tabs = written;
  let model;
  // Each tab's rules as the engine was built with them.
  let ruled = [];
  // Each chart as it was read since the last build, by "sheet index".
  const charted = new Map();
  const past = [];
  const future = [];

  // The tabs as an engine's workbook. `scratch` adds the sheet this works
  // on, which a workbook sent out does not carry.
  const made = (scratch) => {
    const model = new engine.Model("Sheet", "en", "UTC", "en");
    model.pauseEvaluation();
    // Named as it is made: a later tab may bear the next default name.
    tabs.forEach((tab, sheet) => {
      if (sheet) model.newSheet();
      model.renameSheet(sheet, tab.name);
    });
    if (scratch) {
      let name = SCRATCH;
      while (tabs.some((tab) => tab.name === name)) name += SCRATCH;
      model.newSheet();
      model.renameSheet(tabs.length, name);
    }
    const format = (pattern, sheet, row, column, height = 1) => {
      if (pattern && height)
        model.updateRangeStyle(
          { sheet, row, column, width: 1, height },
          "num_fmt",
          pattern,
        );
    };
    const patternOf = (cell, sheet, column) =>
      cell.attrs["data-format"] ??
      (cell.text.startsWith("=") &&
        tabs[sheet].columns[column - 1]["data-format"]);
    tabs.forEach((tab, sheet) =>
      tab.columns.forEach((column, c) =>
        format(column["data-format"], sheet, 1, c + 1, tab.rows.length),
      ),
    );
    // Every pattern before any input: a formula shows as the cell it reads
    // first does, wherever that lies.
    eachCell(tabs, (cell, sheet, row, column) =>
      format(cell.attrs["data-format"], sheet, row, column),
    );
    eachCell(tabs, (cell, sheet, row, column) => {
      if (cell.text) model.setUserInput(sheet, row, column, cell.text);
    });
    // That takes a cell's own pattern and a formula's too, which are set
    // again; over a column's, what a typed value brought ("4.5%", a date)
    // stands.
    eachCell(tabs, (cell, sheet, row, column) =>
      format(patternOf(cell, sheet, column), sheet, row, column),
    );
    tabs.forEach((tab, sheet) => {
      const [top, bottom] = [headOf(tab) + 1, tab.rows.length];
      const { rules, unread } = ruled[sheet];
      for (const rule of top > bottom ? [] : rules) {
        const input = ruleInput(rule, top, bottom, tab.columns.length);
        // A rule the engine refuses is said like one not understood.
        try {
          model.addConditionalFormatting(sheet, input.range, input.rule);
        } catch {
          if (!unread.includes(rule.text)) unread.push(rule.text);
        }
      }
    });
    model.resumeEvaluation();
    model.evaluate();
    return model;
  };
  const build = () => {
    model?.free();
    ruled = tabs.map(rulesOf);
    charted.clear();
    model = made(true);
  };
  build();

  // True for a step made, false for none, or why it was refused.
  const commit = (made) => {
    // Blank cells put in at the end and cut again are no change.
    const next = made.map((tab, at) => {
      const cut = tab === tabs[at] ? tab : trimmed(tab);
      return tableText(cut) === tableText(tabs[at]) ? tabs[at] : cut;
    });
    const changed = next.filter((tab, at) => tab !== tabs[at]);
    const [refused] = locked ? [LOCKED] : changed.flatMap(markupOf);
    if (refused || !changed.length) {
      // What led here may have moved the engine's rows already.
      if (made.some((tab, at) => tab !== tabs[at])) build();
      return changed.length ? refused : false;
    }
    return taken(next);
  };
  const taken = (next) => {
    past.push(tabs);
    if (past.length > STEPS) past.shift();
    future.length = 0;
    tabs = next;
    build();
    return true;
  };
  // A name no tab has yet: itself, or with the first number that makes it so.
  const unused = (name) => {
    const has = (said) =>
      tabs.some((tab) => tab.name.toLowerCase() === said.toLowerCase());
    for (let n = 1; ; n++) {
      const said = n > 1 ? `${name.slice(0, 28)} ${n}` : name;
      if (!has(said)) return said;
    }
  };
  // The tabs themselves put in or taken out: a step, as any change is.
  const retabbed = (next) => (locked ? LOCKED : taken(next));
  // Why text set at `row`, `column` cannot be kept, if so: every blank row
  // and cell between it and the table would be written into the file.
  const sizes = new WeakMap();
  const sized = (tab) =>
    sizes.get(tab) ?? sizes.set(tab, bytes(tableText(tab))).get(tab);
  const tooFar = (sheet, { row, column }) => {
    const { rows, columns } = tabs[sheet];
    const wider = Math.max(0, column - columns.length);
    const more =
      Math.max(0, row - rows.length) * (5 + 4 * (columns.length + wider)) +
      rows.length * 4 * wider;
    return more && tabs.reduce((all, tab) => all + sized(tab), more) > TABLES
      ? `${cellName({ row, column })} is too far from the table to keep: the file would hold every row between.`
      : undefined;
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
  // How a cell shows but for the rules: its own marks over its row's and
  // column's. A header and a total are bold.
  const plain = (sheet, row, column) => {
    const line = tabs[sheet].rows[row - 1];
    const cell = line?.cells[column - 1];
    if (!cell) return {};
    return {
      bold: has(cell.attrs, "bold") || isHeader(line) || isTotal(line),
      italic: has(cell.attrs, "italic"),
      wrap:
        has(cell.attrs, "wrap") || has(tabs[sheet].columns[column - 1], "wrap"),
      align: WORDS.align.find((word) => has(cell.attrs, word)) ?? "",
      fill: cell.attrs["data-fill"] ?? line.attrs["data-fill"],
      ink: cell.attrs["data-ink"],
    };
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

  // A chart's labels and series, or why it cannot be drawn. A blank cell is
  // a gap, and a column without a number no series.
  const readChart = (sheet, { attrs }) => {
    const tab = tabs[sheet];
    const kind = attrs["data-chart"];
    if (!Object.hasOwn(CHARTS, kind)) return { error: "is of no known kind" };
    // Past the table its range reads blank cells: the grid runs on there.
    const box = boxNamed(attrs["data-range"]);
    if (!box) return { error: "names no cells" };
    const rows = Array.from(
      { length: box.bottom - box.top },
      (_, at) => box.top + 1 + at,
    );
    const columns = Array.from(
      { length: box.right - box.left },
      (_, at) => box.left + 1 + at,
    );
    const cells = columns.flatMap((column) =>
      rows.map((row) => ({
        top: row,
        left: column,
        bottom: row,
        right: column,
      })),
    );
    const values = worked(cells.map((cell) => `=${where(tab.name, cell)}`)).map(
      ({ kind, value }, at) =>
        kind === "number" &&
        tab.rows[cells[at].top - 1]?.cells[cells[at].left - 1]?.text
          ? value
          : undefined,
    );
    const series = columns
      .map((column, at) => ({
        name: model.getFormattedCellValue(sheet, box.top, column),
        values: values.slice(at * rows.length, (at + 1) * rows.length),
      }))
      .filter(({ values }) => values.some((value) => value !== undefined));
    const drawn =
      kind === "pie"
        ? series[0]?.values.some((value) => value > 0)
        : series.length > 0;
    return drawn
      ? {
          labels: rows.map((row) =>
            model.getFormattedCellValue(sheet, row, box.left),
          ),
          series,
        }
      : { error: "holds no numbers" };
  };
  const chartOf = (sheet, index) => {
    const key = `${sheet} ${index}`;
    if (!charted.has(key))
      charted.set(key, readChart(sheet, tabs[sheet].charts[index]));
    return charted.get(key);
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
    xlsx() {
      const workbook = made(false);
      try {
        tabs.forEach((tab, sheet) => {
          const held = frozenOf(tab.attrs["data-frozen"], headOf(tab));
          workbook.setFrozenRowsCount(sheet, held.rows);
          workbook.setFrozenColumnsCount(sheet, held.columns);
          tab.columns.forEach((column, c) => {
            const wide = Number(column["data-width"]);
            if (wide) workbook.setColumnsWidth(sheet, c + 1, c + 1, wide);
          });
        });
        // A rule's look goes out as the rule; what is written, as a style.
        eachCell(tabs, (_, sheet, row, column) => {
          const area = { sheet, row, column, width: 1, height: 1 };
          for (const [name, value] of Object.entries(
            plain(sheet, row, column),
          )) {
            const said = COLOURED[name] ? COLOURED[name][value] : `${value}`;
            if (value && said)
              workbook.updateRangeStyle(area, STYLED[name], said);
          }
        });
        return workbook.toXlsx();
      } finally {
        workbook.free();
      }
    },
    shown: (sheet, row, column) =>
      model.getFormattedCellValue(sheet, row, column),
    kind: (sheet, row, column) =>
      KINDS[model.getCellType(sheet, row, column)] ?? "text",

    // Whether a rule can make a cell of the column bold.
    emboldens: (sheet, column) =>
      ruled[sheet].rules.some(
        (rule) => rule.format.font.b && (rule.row || rule.column === column),
      ),
    // What a rule that matches gives a cell. A tab without rules does not
    // ask the engine for every cell's style.
    look(sheet, row, column) {
      if (!ruled[sheet].rules.length) return {};
      const { fill, font } = model.getCellStyle(sheet, row, column).style;
      return {
        fill: FILLED[String(fill.color).toUpperCase()],
        bold: !!font.b,
        strike: !!font.strike,
      };
    },

    plain,
    // How a cell shows: what is written, and a rule's look over it.
    marks(sheet, row, column) {
      const own = plain(sheet, row, column);
      const look = this.look(sheet, row, column);
      return {
        ...own,
        bold: !!(own.bold || look.bold),
        strike: !!look.strike,
        fill: look.fill ?? own.fill,
      };
    },
    // The same for the controls that set them, with what the engine knows:
    // a number stands right, in a pattern ("general" for none).
    state(sheet, row, column) {
      const marks = this.marks(sheet, row, column);
      return {
        ...marks,
        align:
          marks.align ||
          (this.kind(sheet, row, column) === "number" ? "right" : "left"),
        format: model.getCellStyle(sheet, row, column).style.num_fmt,
      };
    },

    chart: chartOf,
    setCharts: (sheet, change) =>
      commit(edit(sheet, (tab) => ({ ...tab, charts: change(tab.charts) }))),

    errors() {
      const found = tabs.flatMap((tab, sheet) => [
        ...markupOf(tab),
        ...ruled[sheet].unread.map(
          (rule) => `${tab.name}: rule \`${rule}\` is not understood`,
        ),
        ...tab.charts.flatMap((chart, at) => {
          const { error } = chartOf(sheet, at);
          return error
            ? [`${tab.name}: chart ${chart.attrs["data-range"]} ${error}`]
            : [];
        }),
      ]);
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

    addTab: () =>
      retabbed([
        ...tabs,
        {
          name: unused("Sheet"),
          attrs: {},
          markup: [],
          charts: [],
          columns: [],
          rows: [],
        },
      ]),
    copyTab: (sheet) =>
      retabbed(
        tabs.toSpliced(sheet + 1, 0, {
          ...tabs[sheet],
          name: unused(`${tabs[sheet].name.slice(0, 26)} copy`),
        }),
      ),
    dropTab: (sheet) =>
      tabs.length > 1
        ? retabbed(tabs.toSpliced(sheet, 1))
        : "A sheet keeps at least one tab.",
    moveTab(sheet, by) {
      const to = sheet + by;
      return to >= 0 && to < tabs.length
        ? commit(tabs.with(sheet, tabs[to]).with(to, tabs[sheet]))
        : false;
    },
    // The engine writes the new name into the formulas that read the tab.
    nameTab(sheet, said) {
      const name = said.trim();
      if (name === tabs[sheet].name) return false;
      if (
        !name ||
        name.length > 31 ||
        /[[\]:*?/\\]/.test(name) ||
        tabs.some(
          (tab, at) =>
            at !== sheet && tab.name.toLowerCase() === name.toLowerCase(),
        )
      )
        return "A tab's name is its own, at most 31 characters, without [ ] : * ? / \\.";
      return restructured(
        () => model.renameSheet(sheet, name),
        tabs.with(sheet, { ...tabs[sheet], name }),
      );
    },
    // How many cells of other tabs read this one.
    readers(sheet) {
      const names = [tabs[sheet].name, engine.quoteName(tabs[sheet].name)];
      let count = 0;
      eachCell(tabs, (cell, on) => {
        if (
          on !== sheet &&
          cell.text.startsWith("=") &&
          names.some((name) => cell.text.includes(`${name}!`))
        )
          count++;
      });
      return count;
    },

    // A mark set on the tab's table: `data-frozen="2 B"`.
    mark: (sheet, name, value) =>
      commit(
        tabs.with(sheet, {
          ...tabs[sheet],
          attrs: { ...tabs[sheet].attrs, [name]: value },
        }),
      ),

    // A look set on the box's cells but those in `skip`, which the table
    // reaches for. A word is taken out where every cell has it; an
    // attribute goes with "".
    style(sheet, box, name, value, skip) {
      const words = WORDS[name];
      const may = words ?? (SAID[name] && ["", ...SAID[name]]);
      if (may && !may.includes(value)) return false;
      const tab = tabs[sheet];
      const attr = words ? "class" : `data-${name}`;
      const inside = (row, column) =>
        row >= box.top &&
        row <= box.bottom &&
        column >= box.left &&
        column <= box.right &&
        !skip.has(row);
      const off = words
        ? box.bottom <= tab.rows.length &&
          box.right <= tab.columns.length &&
          tab.rows.every((row, r) =>
            row.cells.every(
              (cell, c) => !inside(r + 1, c + 1) || has(cell.attrs, value),
            ),
          )
        : !value;
      const refused =
        !off && tooFar(sheet, { row: box.bottom, column: box.right });
      if (refused) return refused;
      return commit(
        mapCells(
          edit(sheet, () => (off ? tab : reaching(tab, box.bottom, box.right))),
          (cell, at, row, column) => {
            if (at !== sheet || !inside(row, column)) return cell;
            const { [attr]: was = "", ...rest } = cell.attrs;
            const now = words
              ? [
                  ...was.split(/\s+/).filter((word) => !words.includes(word)),
                  ...(off ? [] : [value]),
                ]
                  .join(" ")
                  .trim()
              : value;
            if (now === was) return cell;
            return {
              ...cell,
              attrs: now ? { ...cell.attrs, [attr]: now } : rest,
            };
          },
        ),
      );
    },

    // Text set down from a cell on. From this book's own cells (`from`)
    // formulas move along, and a cut clears.
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
      // As far as text is set, the table reaches.
      const far = { row: 0, column: 0 };
      rows.forEach((cells, r) =>
        cells.forEach((text, c) => {
          const [row, column] = [at.row + r, at.column + c];
          texts.set(`${row},${column}`, by ? relocated(text, by) : text);
          if (text) far.row = Math.max(far.row, row);
          if (text) far.column = Math.max(far.column, column);
        }),
      );
      const refused = tooFar(sheet, far);
      if (refused) return refused;
      return placed(
        "pasted",
        mapCells(
          tabs.with(sheet, reaching(tabs[sheet], far.row, far.column)),
          (cell, on, row, column) =>
            on === sheet && texts.has(`${row},${column}`)
              ? withText(cell, texts.get(`${row},${column}`))
              : cell,
        ),
      );
    },

    // One row alone takes the row above it.
    fillDown(sheet, box) {
      const top = box.bottom > box.top ? box.top : box.top - 1;
      const source = tabs[sheet].rows[top - 1]?.cells;
      if (!source) return false;
      const filled = source
        .slice(box.left - 1, box.right)
        .some(({ text }) => text);
      const refused =
        filled && tooFar(sheet, { row: box.bottom, column: box.left });
      if (refused) return refused;
      return placed(
        "filled",
        mapCells(
          tabs.with(sheet, reaching(tabs[sheet], filled ? box.bottom : 0, 0)),
          (cell, at, row, column) =>
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
          charts: slidCharts(tab.charts, false, at, count),
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
          charts: slidCharts(tab.charts, false, at, -count),
          rows: tab.rows.toSpliced(at - 1, count),
        })),
      ),
    insertColumns: (sheet, at, count) =>
      restructured(
        () => model.insertColumns(sheet, at, count),
        edit(sheet, (tab) => ({
          ...tab,
          attrs: slidAttrs(tab, at, count),
          charts: slidCharts(tab.charts, true, at, count),
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
          attrs: slidAttrs(tab, at, -count),
          charts: slidCharts(tab.charts, true, at, -count),
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
