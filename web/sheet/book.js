// A sheet's tabs as its file keeps them. What was typed is the record: the
// engine reads "4.5%" as 0.045, so it only calculates and moves formulas.

import { CHARTS } from "./chart.js";
import {
  FILLS,
  readRules,
  ruleInput,
  rulesAt,
  slidRules,
  withRule,
} from "./rules.js";
import {
  addMerge,
  boxName,
  boxNamed,
  cellName,
  followed,
  frozenOf,
  inOrder,
  LAST,
  letters,
  linesName,
  linesNamed,
  meeting,
  meets,
  mergesBy,
  moved,
  renamed,
  slid,
  slidBox,
  SPARE,
  tableText,
} from "./text.js";

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
// A run of a list `by` places on, and where each place of it then lies.
const lifted = (list, at, count, by) => {
  const next = [...list];
  next.splice(at - 1 + by, 0, ...next.splice(at - 1, count));
  return next;
};
const lifting = (at, count, by) => (n) =>
  n >= at && n < at + count
    ? n + by
    : n >= Math.min(at, at + by) && n < Math.max(at, at + by) + count
      ? n - Math.sign(by) * count
      : n;

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

// Charts after cells moved, each to where `place` puts it: a chart lies on
// its cell, and its range runs between where its two ends went, so what it
// drew is still in it.
const carriedCharts = (charts, place) =>
  charts.map((chart) => {
    const range = boxNamed(chart.attrs["data-range"]);
    const on = boxNamed(chart.attrs["data-at"]);
    const ends = range && [
      place({ row: range.top, column: range.left }),
      place({ row: range.bottom, column: range.right }),
    ];
    const most = (way, of) => of(...ends.map((end) => end[way]));
    return {
      ...chart,
      attrs: {
        ...chart.attrs,
        ...(ends && {
          "data-range": boxName({
            top: most("row", Math.min),
            left: most("column", Math.min),
            bottom: most("row", Math.max),
            right: most("column", Math.max),
          }),
        }),
        ...(on && {
          "data-at": cellName(place({ row: on.top, column: on.left })),
        }),
      },
    };
  });

// A list on a table, its merged ranges `data-merge="A1:D1 A5:A7"` unless
// named, as written.
const listed = (attrs, name = "data-merge") =>
  (attrs[name] ?? "").split(/\s+/).filter(Boolean);
// The cells of a box but its top left one that hold text, by name.
const filledUnder = (tab, box) => {
  const found = [];
  const right = Math.min(box.right, tab.columns.length);
  for (let row = box.top; row <= Math.min(box.bottom, tab.rows.length); row++)
    for (let column = box.left; column <= right; column++)
      if (
        (row > box.top || column > box.left) &&
        tab.rows[row - 1].cells[column - 1].text
      )
        found.push(cellName({ row, column }));
  return found;
};
// The merged ranges drawn, each as its top left cell over the others, which
// hold nothing; and why each other is not, said to the agent and to a change
// that would leave it so. Of two that overlap the first is drawn.
const mergesOf = (tab) => {
  const merges = mergesBy(tab.rows.length);
  const unread = [];
  for (const [place, text] of listed(tab.attrs).entries()) {
    const box = boxNamed(text);
    const over = box && meeting(merges, box).next().value;
    const [under] = box && !over ? filledUnder(tab, box) : [];
    const why = !box
      ? "names no cells"
      : !boxName(box).includes(":")
        ? "is one cell"
        : over
          ? `overlaps ${boxName(over)}`
          : under && `covers text in ${under}`;
    if (!why) addMerge(merges, box);
    else
      unread.push({
        place,
        said: `merge \`${text}\` ${why}`,
        refused: under
          ? `Not changed: ${under} lies under the merged cell ${boxName(box)}.`
          : `Not changed: merge \`${text}\` ${why}.`,
      });
  }
  return { merges, unread };
};
const withList = (attrs, list, name = "data-merge") => {
  const { [name]: _, ...rest } = attrs;
  return list.length ? { ...attrs, [name]: list.join(" ") } : rest;
};
// The merged ranges listed but those that meet the box.
const apart = (attrs, box) =>
  listed(attrs).filter((text) => {
    const merge = boxNamed(text);
    return !merge || !meets(merge, box);
  });
const MOVED = "A merged cell moves whole.";
// Where the numbers from `from` to `to` go, from the least to the most.
const ends = (from, to, go) => {
  let [least, most] = [Infinity, -Infinity];
  for (let n = from; n <= to; n++)
    [least, most] = [Math.min(least, go(n)), Math.max(most, go(n))];
  return [least, most];
};
// The merged ranges and groups after cells moved to where `place` puts
// them, and the first of each the move would cut: its rows or columns would
// be apart, or a merged range's top left cell not first. Within one, the
// cells it holds may move.
const carriedMerges = (attrs, place) => {
  let group;
  let cut;
  const list = listed(attrs).map((text) => {
    const box = boxNamed(text);
    if (!box) return text;
    const [top, bottom] = ends(
      box.top,
      box.bottom,
      (row) => place({ row, column: box.left }).row,
    );
    const [left, right] = ends(
      box.left,
      box.right,
      (column) => place({ row: box.top, column }).column,
    );
    const first = place({ row: box.top, column: box.left });
    if (
      first.row !== top ||
      first.column !== left ||
      bottom - top !== box.bottom - box.top ||
      right - left !== box.right - box.left
    )
      cut ??= text;
    return boxName({ top, left, bottom, right });
  });
  const lines = regrouped(withList(attrs, list), (lines) => {
    const [from, to] = ends(lines.from, lines.to, (n) =>
      lines.across
        ? place({ row: 1, column: n }).column
        : place({ row: n, column: 1 }).row,
    );
    if (to - from !== lines.to - lines.from) group ??= linesName(lines);
    return { ...lines, from, to };
  });
  return { attrs: lines, cut, group };
};

// A table's groups, `data-group="2:12 3:6 B:D"`, by rows and by columns,
// each as deep as the groups that hold it make it and folded where
// `data-folded` names it; and why each other is not read. Of two that cut
// or touch, the first is read: a group's button stands on the row or column
// after it.
const GROUPS = ["data-group", "data-folded"];
const DEEPEST = 7;
const holds = (a, b) =>
  a.across === b.across && a.from <= b.from && b.to <= a.to;
const groupsOf = (tab) => {
  const [read, unread] = [[], []];
  for (const text of listed(tab.attrs, "data-group")) {
    const lines = linesNamed(text);
    const other =
      lines &&
      read.find(
        (group) =>
          group.across === lines.across &&
          group.from <= lines.to + 1 &&
          lines.from <= group.to + 1 &&
          holds(group, lines) === holds(lines, group),
      );
    const line = lines?.across ? "column" : "row";
    const why = !lines
      ? "names no whole rows or columns"
      : lines.to === LAST[line]
        ? `leaves no ${line} after it for its button`
        : !other
          ? undefined
          : holds(other, lines) && holds(lines, other)
            ? "is listed twice"
            : lines.to < other.from || lines.from > other.to
              ? `touches ${other.name}: leave a ${line} between`
              : `cuts ${other.name}`;
    if (why) unread.push(`group \`${text}\` ${why}`);
    else read.push({ ...lines, name: linesName(lines) });
  }
  const groups = read.filter((group) => {
    group.depth = read.filter((other) => holds(other, group)).length;
    const deep = group.depth > DEEPEST;
    if (deep)
      unread.push(`group \`${group.name}\` is nested over ${DEEPEST} deep`);
    return !deep;
  });
  for (const text of listed(tab.attrs, "data-folded")) {
    const lines = linesNamed(text);
    const group = lines && groups.find(({ name }) => name === linesName(lines));
    if (group) group.folded = true;
    else unread.push(`folded \`${text}\` is no group`);
  }
  // A group in a folded one is not drawn.
  for (const group of groups)
    group.shut = groups.some(
      (other) => other !== group && other.folded && holds(other, group),
    );
  return {
    rows: groups.filter(({ across }) => !across),
    columns: groups.filter(({ across }) => across),
    unread,
  };
};
// The table's groups and folds (or the lists named) each where `change`
// puts its lines, or gone.
const regrouped = (attrs, change, names = GROUPS) =>
  names.reduce(
    (next, name) =>
      withList(
        next,
        listed(attrs, name).flatMap((text) => {
          const lines = linesNamed(text);
          const moved = lines && change(lines);
          return !lines ? [text] : moved ? [linesName(moved)] : [];
        }),
        name,
      ),
    attrs,
  );
const GROUPED = "A group moves whole.";

// A table's rules name columns by letter, its merged ranges cells and its
// groups rows or columns: they follow them. A range left a cell or none is
// no merge, a group left no row none.
const slidAttrs = ({ attrs, columns }, across, at, count) => {
  const merged = regrouped(
    withList(
      attrs,
      listed(attrs).flatMap((text) => {
        const box = boxNamed(text);
        const left = box && slidBox(box, across, at, count);
        if (!box) return [text];
        return left && boxName(left).includes(":") ? [boxName(left)] : [];
      }),
    ),
    (lines) => {
      if (lines.across !== across) return lines;
      const [first, last] = across ? ["left", "right"] : ["top", "bottom"];
      const box = slidBox(
        { [first]: lines.from, [last]: lines.to },
        across,
        at,
        count,
      );
      return box && { across, from: box[first], to: box[last] };
    },
  );
  return across && attrs["data-rule"]
    ? {
        ...merged,
        "data-rule": slidRules(attrs["data-rule"], columns.length, (column) =>
          slid(column, at, count),
        ),
      }
    : merged;
};

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
const STILL =
  "Not changed: the values it reads are still being calculated. Ask again in a moment.";
// The host keeps no file over 1,024 KB: the tabs' tables may come to this.
const TABLES = 1_000_000;
const bytes = (text) => new TextEncoder().encode(text).length;

// The tabs as an engine's workbook, made paused. Only a worker of the page
// calculates one: a formula the engine never returns from holds still the
// thread it calculates on.
export const workbookOf = (engine, tabs, ruled = tabs.map(rulesOf)) => {
  const model = new engine.Model("Sheet", "en", "UTC", "en");
  model.pauseEvaluation();
  // Named as it is made: a later tab may bear the next default name.
  tabs.forEach((tab, sheet) => {
    if (sheet) model.newSheet();
    model.renameSheet(sheet, tab.name);
  });
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
  return model;
};

// A cell past a tab's table, as the engine reads it.
const BLANK = { shown: "", kind: "number", format: "general" };

// What the page reads of the tabs, calculated: for each tab, each cell as
// it shows, its kind and its pattern, and a number to fifteen digits, so it
// sorts and sums exactly; the look a rule gives a cell it is set on; the
// rules not understood. In a worker made for one answer and ended after it:
// the workbook is not freed, which after a panic it could not be.
export function tableOf(engine, tabs) {
  const ruled = tabs.map(rulesOf);
  const model = workbookOf(engine, tabs, ruled);
  model.resumeEvaluation();
  model.evaluate();
  const table = tabs.map((tab, sheet) => {
    const looks = {};
    for (const rule of ruled[sheet].rules)
      for (let row = headOf(tab) + 1; row <= tab.rows.length; row++)
        for (const column of rule.row
          ? tab.columns.map((_, c) => c + 1)
          : [rule.column]) {
          const { fill, font } = model.getCellStyle(sheet, row, column).style;
          looks[`${row} ${column}`] = {
            fill: FILLED[String(fill.color).toUpperCase()],
            bold: !!font.b,
            strike: !!font.strike,
          };
        }
    const cells = tab.rows.map((row, r) =>
      row.cells.map((_, c) => ({
        shown: model.getFormattedCellValue(sheet, r + 1, c + 1),
        kind: KINDS[model.getCellType(sheet, r + 1, c + 1)] ?? "text",
        format: model.getCellStyle(sheet, r + 1, c + 1).style.num_fmt,
      })),
    );
    return { cells, looks, unread: ruled[sheet].unread };
  });
  model.pauseEvaluation();
  table.forEach(({ cells }, sheet) => {
    const [height, width] = [cells.length, tabs[sheet].columns.length];
    if (height && width)
      model.updateRangeStyle(
        { sheet, row: 1, column: 1, width, height },
        "num_fmt",
        PLAIN,
      );
    cells.forEach((row, r) =>
      row.forEach((cell, c) => {
        if (cell.kind === "number" && cell.shown)
          cell.number = Number(
            model.getFormattedCellValue(sheet, r + 1, c + 1),
          );
      }),
    );
  });
  return table;
}

// How a cell shows but for the rules: its own marks over its row's and
// column's. A header and a total are bold.
const plainOf = (tab, row, column) => {
  const line = tab.rows[row - 1];
  const cell = line?.cells[column - 1];
  if (!cell) return {};
  return {
    bold: has(cell.attrs, "bold") || isHeader(line) || isTotal(line),
    italic: has(cell.attrs, "italic"),
    wrap: has(cell.attrs, "wrap") || has(tab.columns[column - 1], "wrap"),
    align: WORDS.align.find((word) => has(cell.attrs, word)) ?? "",
    fill: cell.attrs["data-fill"] ?? line.attrs["data-fill"],
    ink: cell.attrs["data-ink"],
  };
};

// The tabs as an Excel workbook: a rule's look goes out as the rule; what
// is written, as a style; a merged range drawn, as Excel's; a folded
// group's rows or columns, hidden, as the engine keeps no groups.
export function xlsxOf(engine, tabs) {
  const workbook = workbookOf(engine, tabs);
  tabs.forEach((tab, sheet) => {
    const held = frozenOf(tab.attrs["data-frozen"], headOf(tab));
    workbook.setFrozenRowsCount(sheet, held.rows);
    workbook.setFrozenColumnsCount(sheet, held.columns);
    tab.columns.forEach((column, c) => {
      const wide = Number(column["data-width"]);
      if (wide) workbook.setColumnsWidth(sheet, c + 1, c + 1, wide);
    });
  });
  eachCell(tabs, (_, sheet, row, column) => {
    const area = { sheet, row, column, width: 1, height: 1 };
    for (const [name, value] of Object.entries(
      plainOf(tabs[sheet], row, column),
    )) {
      const said = COLOURED[name] ? COLOURED[name][value] : `${value}`;
      if (value && said) workbook.updateRangeStyle(area, STYLED[name], said);
    }
  });
  tabs.forEach((tab, sheet) => {
    for (const { top, left, bottom, right } of mergesOf(tab).merges.all)
      workbook.mergeCells({
        sheet,
        row: top,
        column: left,
        width: right - left + 1,
        height: bottom - top + 1,
      });
    const { rows, columns } = groupsOf(tab);
    for (const { across, from, to, folded } of [...rows, ...columns])
      if (folded)
        workbook[across ? "setColumnsHidden" : "setRowsHidden"](
          sheet,
          from,
          to,
          true,
        );
  });
  workbook.resumeEvaluation();
  workbook.evaluate();
  return workbook.toXlsx();
}

// `locked` for the tabs of an Excel or CSV file: no change is made to them.
// `calculate` answers with what is read of tabs (tableOf), made away from
// the page, or with why not. Nothing where it does not for the tabs written.
export async function openBook(engine, written, locked, calculate) {
  // The tabs and steps as the changes asked for leave them: what the page
  // shows and a change is made on. And those last calculated.
  let tabs = written;
  let past = [];
  let future = [];
  let made = { tabs, past: [], future: [] };
  // What was calculated, by the cell or row it was read of: a cell shows
  // its value wherever the changes since have moved it.
  const values = new WeakMap();
  const looks = new WeakMap();
  const unread = new WeakMap();
  const ruled = new WeakMap();
  const rulesFor = (tab) =>
    ruled.get(tab) ?? ruled.set(tab, rulesOf(tab)).get(tab);
  const merged = new WeakMap();
  const mergesFor = (tab) =>
    merged.get(tab) ?? merged.set(tab, mergesOf(tab)).get(tab);
  const grouped = new WeakMap();
  const groupsFor = (tab) =>
    grouped.get(tab) ?? grouped.set(tab, groupsOf(tab)).get(tab);
  // Each chart as it was read since the tabs last changed, by "sheet index".
  const charted = new Map();
  const learned = (calculated, read) => {
    calculated.forEach((tab, sheet) => {
      unread.set(tab, read[sheet].unread);
      tab.rows.forEach((row, r) =>
        row.cells.forEach((cell, c) =>
          values.set(cell, read[sheet].cells[r][c]),
        ),
      );
      for (const [at, look] of Object.entries(read[sheet].looks)) {
        const [r, c] = at.split(" ").map(Number);
        const row = tab.rows[r - 1];
        const cell = row.cells[c - 1];
        if (cell) looks.set(cell, look);
        else looks.set(row, { ...looks.get(row), [c]: look });
      }
    });
    charted.clear();
  };
  const opened = await calculate(tabs);
  if (typeof opened === "string") return opened;
  learned(tabs, opened);
  // Every read of what was calculated comes through here, which marks the
  // change being made as one that read it.
  let consulted = false;
  const readOf = (cell) => {
    consulted = true;
    if (!cell) return BLANK;
    // Not calculated yet: as typed, a formula blank.
    return (
      values.get(cell) ?? {
        ...BLANK,
        shown: cell.text.startsWith("=") ? "" : cell.text,
      }
    );
  };
  const cellAt = (sheet, row, column) =>
    readOf(tabs[sheet]?.rows[row - 1]?.cells[column - 1]);
  // A formula only rewritten keeps its value until it is calculated again.
  const rewritten = (cell, text) => {
    const next = withText(cell, text);
    if (next !== cell && values.has(cell)) values.set(next, values.get(cell));
    return next;
  };
  // Where nothing moved, a cell or row only given a look keeps what was
  // calculated of it.
  const kept = (before, after) =>
    after.forEach((tab, sheet) =>
      tab.rows.forEach((row, r) => {
        const was = before[sheet]?.rows[r];
        if (!was || was === row) return;
        if (looks.has(was) && !looks.has(row)) looks.set(row, looks.get(was));
        row.cells.forEach((cell, c) => {
          const old = was.cells[c];
          if (!old || old.text !== cell.text || values.has(cell)) return;
          if (values.has(old)) values.set(cell, values.get(old));
          if (looks.has(old)) looks.set(cell, looks.get(old));
        });
      }),
    );
  // A change is made at once on the tabs shown, then calculated after those
  // asked for before it. Where one of those is refused, the tabs go back to
  // those calculated and those behind it are made again on them; where it
  // moved cells (`moves`), what they point at moved, and they are refused
  // too, as Undo and Redo are (`moves` "step"): what they would undo is
  // gone. The app holds the tabs shown, and writes them if the page goes
  // first.
  let pending = Promise.resolve();
  const waiting = [];
  // `asked` made on the tabs shown: true, false for no change, or why not.
  const attempt = (asked) => {
    const was = [tabs, [...past], [...future]];
    consulted = false;
    let answer;
    try {
      answer = asked.make();
      // A change that reads values not yet calculated would read others.
      if (consulted && was[0] !== made.tabs) answer = STILL;
    } catch (error) {
      answer = `Not changed: ${error.message}`;
    }
    if (answer === true) {
      if (!asked.moves) kept(was[0], tabs);
      asked.to = { tabs, past: [...past], future: [...future] };
      charted.clear();
    } else [tabs, past, future] = was;
    return answer;
  };
  const queued =
    (change, moves = false) =>
    (...args) => {
      const asked = { make: () => change(...args), moves };
      const answer = attempt(asked);
      if (answer !== true) return Promise.resolve(answer);
      waiting.push(asked);
      return (pending = pending
        .catch(() => {})
        .then(async () => {
          try {
            if (asked.dropped) return false;
            const read = await calculate(asked.to.tabs);
            if (typeof read !== "string") {
              made = asked.to;
              learned(made.tabs, read);
              return true;
            }
            [tabs, past, future] = [
              made.tabs,
              [...made.past],
              [...made.future],
            ];
            let [moved, dropped] = [asked.moves, 0];
            for (const next of waiting.slice(1)) {
              if (next.dropped) continue;
              const again = !moved && next.moves !== "step" && attempt(next);
              if (again === true) continue;
              next.dropped = true;
              dropped++;
              moved ||= !!next.moves;
            }
            charted.clear();
            return dropped
              ? `${read} ${dropped === 1 ? "A change" : `${dropped} changes`} asked for after it ${dropped === 1 ? "was" : "were"} not made either.`
              : read;
          } finally {
            waiting.shift();
          }
        }));
    };
  // A change that moves cells or tabs.
  const moving = (change) => queued(change, true);

  // True for a step made, false for none, or why it was refused.
  const commit = (made) => {
    // Blank cells put in at the end and cut again are no change.
    const next = made.map((tab, at) => {
      const cut = tab === tabs[at] ? tab : trimmed(tab);
      return tableText(cut) === tableText(tabs[at]) ? tabs[at] : cut;
    });
    const changed = next.filter((tab, at) => tab !== tabs[at]);
    // No text goes under a merged cell: a change is refused where it leaves
    // one of a tab's merged ranges as written, in its place in the list,
    // drawn before and not after. A change that moves cells takes their
    // ranges along, and a merge or an unmerge covers none.
    const covered = next.flatMap((tab, at) => {
      const [was, now] = [listed(tabs[at].attrs), listed(tab.attrs)];
      const undrawn = new Set(
        mergesFor(tabs[at]).unread.map(({ place }) => place),
      );
      return mergesFor(tab).unread.filter(
        ({ place }) => was[place] === now[place] && !undrawn.has(place),
      );
    });
    // Nor where it leaves more groups unread than there were.
    const unread = (all) => all.flatMap((tab) => groupsFor(tab).unread);
    const [was, now] = [unread(tabs), unread(next)];
    const [refused] = locked
      ? [LOCKED]
      : [
          ...changed.flatMap(markupOf),
          ...covered.map((one) => one.refused),
          ...now
            .filter((said) => now.length > was.length && !was.includes(said))
            .map((said) => `Not changed: ${said}.`),
        ];
    if (refused || !changed.length) return changed.length ? refused : false;
    return taken(next);
  };
  const taken = (next) => {
    past.push(tabs);
    if (past.length > STEPS) past.shift();
    future.length = 0;
    tabs = next;
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
    return true;
  };
  const named = (sheet, name) => {
    const { rows, columns } = groupsFor(tabs[sheet]);
    return [...rows, ...columns].find((group) => group.name === name);
  };
  const regroup = (sheet, change) =>
    commit(
      tabs.with(sheet, { ...tabs[sheet], attrs: change(tabs[sheet].attrs) }),
    );
  const added = (list, name) => (attrs) =>
    withList(attrs, [...listed(attrs, list), name], list);
  const dropped = (lists, name) => (attrs) =>
    regrouped(attrs, (lines) => linesName(lines) !== name && lines, lists);
  const edit = (sheet, change) =>
    tabs.map((tab, at) => (at === sheet ? change(tab) : tab));
  // Rows or columns moved by the engine, in a workbook made for the move and
  // not calculated. A formula takes the engine's text only where the move
  // changed it: the engine rewrites "=1e3" as "=1000".
  const restructured = (move, next) => {
    const model = workbookOf(engine, tabs);
    try {
      const was = new Map();
      eachCell(tabs, (cell, sheet, row, column) => {
        if (cell.text.startsWith("="))
          was.set(cell, model.getCellContent(sheet, row, column));
      });
      move(model);
      return commit(
        mapCells(next, (cell, sheet, row, column) => {
          const now = was.has(cell) && model.getCellContent(sheet, row, column);
          return now && now !== was.get(cell) ? rewritten(cell, now) : cell;
        }),
      );
    } finally {
      model.free();
    }
  };
  // The cells of a tab moved to where `place` puts them, in `next`: the
  // formulas that read one of them read it there.
  const following = (sheet, place, next) =>
    commit(
      mapCells(next, (cell, on) =>
        cell.text.startsWith("=")
          ? rewritten(
              cell,
              followed(
                cell.text,
                spans(cell.text),
                tabs[sheet].name,
                tabs[on].name,
                place,
              ),
            )
          : cell,
      ),
    );
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
    const values = cells.map(({ top, left }) =>
      tab.rows[top - 1]?.cells[left - 1]?.text
        ? cellAt(sheet, top, left).number
        : undefined,
    );
    const series = columns
      .map((column, at) => ({
        name: cellAt(sheet, box.top, column).shown,
        values: values.slice(at * rows.length, (at + 1) * rows.length),
      }))
      .filter(({ values }) => values.some((value) => value !== undefined));
    const drawn =
      kind === "pie"
        ? series[0]?.values.some((value) => value > 0)
        : series.length > 0;
    return drawn
      ? {
          labels: rows.map((row) => cellAt(sheet, row, box.left).shown),
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
    // The tabs last calculated.
    get made() {
      return made.tabs;
    },
    // Once every change asked for so far is calculated, or refused.
    settled: () => pending,
    canUndo: () => past.length > 0,
    canRedo: () => future.length > 0,
    undo: queued(() => step(past, future), "step"),
    redo: queued(() => step(future, past), "step"),
    table: (sheet) => tableText(tabs[sheet]),
    shown: (sheet, row, column) => cellAt(sheet, row, column).shown,
    kind: (sheet, row, column) => cellAt(sheet, row, column).kind,
    // A formula as the engine reads it, token by token.
    tokens: engine.getTokens,

    // Whether a rule can make a cell of the column bold.
    emboldens: (sheet, column) =>
      rulesFor(tabs[sheet]).rules.some(
        (rule) => rule.format.font.b && (rule.row || rule.column === column),
      ),
    // What a rule that matches gives a cell.
    look(sheet, row, column) {
      const line = tabs[sheet].rows[row - 1];
      return (
        looks.get(line?.cells[column - 1]) ?? looks.get(line)?.[column] ?? {}
      );
    },

    plain: (sheet, row, column) => plainOf(tabs[sheet], row, column),
    // The tab's merged ranges that are drawn (mergesBy).
    merges: (sheet) => mergesFor(tabs[sheet]).merges,
    // How a cell shows: what is written, and a rule's look over it.
    marks(sheet, row, column) {
      const own = this.plain(sheet, row, column);
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
        format: cellAt(sheet, row, column).format,
      };
    },

    chart: chartOf,
    setCharts: queued((sheet, change) =>
      commit(edit(sheet, (tab) => ({ ...tab, charts: change(tab.charts) }))),
    ),

    errors() {
      const found = tabs.flatMap((tab, sheet) => [
        ...markupOf(tab),
        ...(unread.get(tab) ?? rulesFor(tab).unread).map(
          (rule) => `${tab.name}: rule \`${rule}\` is not understood`,
        ),
        ...mergesFor(tab).unread.map(({ said }) => `${tab.name}: ${said}`),
        ...groupsFor(tab).unread.map((said) => `${tab.name}: ${said}`),
        ...tab.charts.flatMap((chart, at) => {
          const { error } = chartOf(sheet, at);
          return error
            ? [`${tab.name}: chart ${chart.attrs["data-range"]} ${error}`]
            : [];
        }),
      ]);
      eachCell(tabs, (cell, sheet, row, column) => {
        const { kind, shown } = cellAt(sheet, row, column);
        if (kind === "error")
          found.push(
            `${tabs[sheet].name}!${letters(column)}${row} ${cell.text} is ${shown}`,
          );
      });
      return found.slice(0, 20);
    },

    // Nothing for fewer than two numbers, or with a cell in error.
    summary(sheet, { top, left, bottom, right }) {
      const read = tabs[sheet].rows
        .slice(top - 1, bottom)
        .flatMap((row) => row.cells.slice(left - 1, right).map(readOf));
      const numbers = read.flatMap((cell) => cell.number ?? []);
      const sum = numbers.reduce((all, number) => all + number, 0);
      return numbers.length > 1 && !read.some((cell) => cell.kind === "error")
        ? { count: numbers.length, sum, average: sum / numbers.length }
        : undefined;
    },

    addTab: moving(() =>
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
    ),
    copyTab: moving((sheet) =>
      retabbed(
        tabs.toSpliced(sheet + 1, 0, {
          ...tabs[sheet],
          name: unused(`${tabs[sheet].name.slice(0, 26)} copy`),
          // Rows and cells of its own: what was calculated is kept by them.
          rows: tabs[sheet].rows.map((row) => ({
            ...row,
            cells: row.cells.map((cell) => ({ ...cell })),
          })),
        }),
      ),
    ),
    dropTab: moving((sheet) =>
      tabs.length > 1
        ? retabbed(tabs.toSpliced(sheet, 1))
        : "A sheet keeps at least one tab.",
    ),
    moveTab: moving((sheet, by) => {
      const to = sheet + by;
      return to >= 0 && to < tabs.length
        ? commit(tabs.with(sheet, tabs[to]).with(to, tabs[sheet]))
        : false;
    }),
    // The new name is written into the formulas that read the tab.
    nameTab: queued((sheet, said) => {
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
      const quoted = engine.quoteName(name);
      return commit(
        mapCells(tabs.with(sheet, { ...tabs[sheet], name }), (cell) =>
          cell.text.startsWith("=")
            ? rewritten(
                cell,
                renamed(cell.text, spans(cell.text), tabs[sheet].name, quoted),
              )
            : cell,
        ),
      );
    }),
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

    rules: (sheet, column) => rulesAt(tabs[sheet], column),
    // The rule at `at` of a column's, which reads `text` there, set to
    // `rule` or taken out without one. For a rule of its own the table
    // reaches the column; one that would not be understood is not written.
    setRule: queued((sheet, column, at, text, rule) => {
      const width = tabs[sheet].columns.length;
      if (rulesAt(tabs[sheet], column)[at]?.text !== text)
        return "Not changed: the column's rules are no longer the ones shown.";
      if (rule?.row && column > width + SPARE.columns)
        return `Not changed: column ${letters(column)} is too far from the table for a rule on its rows.`;
      const far =
        rule?.row === false ? { row: 1, column } : { row: 0, column: 0 };
      const tab = withRule(
        reaching(tabs[sheet], far.row, far.column),
        column,
        at,
        rule,
        width,
      );
      const read =
        !rule ||
        rulesAt(tab, column).some(
          ({ text, condition }) => text === rule.text && condition,
        );
      return (
        tooFar(sheet, far) ??
        (read
          ? commit(tabs.with(sheet, tab))
          : `Not changed: the sheet does not understand the rule ${rule.text}`)
      );
    }),

    // How many cells of the box but its top left one hold text.
    filled: (sheet, box) => filledUnder(tabs[sheet], box).length,
    // The box's cells as one, over the merged ranges it meets: what all but
    // its top left cell hold is taken out, where `emptied` of them held text
    // when the person was asked.
    merge: queued((sheet, box, emptied) => {
      const tab = tabs[sheet];
      const under = new Set(filledUnder(tab, box));
      if (!boxName(box).includes(":")) return false;
      if (under.size !== emptied)
        return "Not merged: its cells hold other text than when asked.";
      const list = [...apart(tab.attrs, box), boxName(box)];
      return commit(
        mapCells(
          tabs.with(sheet, { ...tab, attrs: withList(tab.attrs, list) }),
          (cell, at, row, column) =>
            at === sheet && under.has(cellName({ row, column }))
              ? withText(cell, "")
              : cell,
        ),
      );
    }),
    // The merged ranges the box meets, apart again: the text stays where
    // it is.
    unmerge: queued((sheet, box) => {
      const tab = tabs[sheet];
      const attrs = withList(tab.attrs, apart(tab.attrs, box));
      return commit(tabs.with(sheet, { ...tab, attrs }));
    }),

    // The tab's groups that are read (groupsOf).
    groups: (sheet) => groupsFor(tabs[sheet]),
    // Lines named "3:6" or "B:D" made a group, a group taken out with its
    // fold, or folded or opened. Nothing where it is so already.
    group: queued(
      (sheet, name) =>
        !named(sheet, name) && regroup(sheet, added("data-group", name)),
    ),
    ungroup: queued(
      (sheet, name) =>
        !!named(sheet, name) && regroup(sheet, dropped(GROUPS, name)),
    ),
    fold: queued((sheet, name, folded) => {
      const group = named(sheet, name);
      return (
        !!group &&
        !group.folded === folded &&
        regroup(
          sheet,
          folded ? added("data-folded", name) : dropped(["data-folded"], name),
        )
      );
    }),

    // A mark set on the tab's table: `data-frozen="2 B"`.
    mark: queued((sheet, name, value) =>
      commit(
        tabs.with(sheet, {
          ...tabs[sheet],
          attrs: { ...tabs[sheet].attrs, [name]: value },
        }),
      ),
    ),

    // A look set on the box's cells but those in `skip`, which the table
    // reaches for. A word is taken out where every cell has it; an
    // attribute goes with "".
    style: queued((sheet, box, name, value, skip) => {
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
    }),

    // The looks set on the box's cells, but those in `skip`, taken off; a
    // column selected whole gives up its own pattern and wrap too.
    unstyle: queued((sheet, box, skip) => {
      const looks = Object.values(WORDS).flat();
      const plainly = (attrs, words, names) => {
        const next = { ...attrs };
        for (const name of names) delete next[name];
        const left = (attrs.class ?? "")
          .split(/\s+/)
          .filter((word) => word && !words.includes(word));
        if (left.length) next.class = left.join(" ");
        else delete next.class;
        return JSON.stringify(next) === JSON.stringify(attrs) ? attrs : next;
      };
      const whole = box.top <= 1 && box.bottom >= tabs[sheet].rows.length;
      const within = (column) => column >= box.left && column <= box.right;
      // A row's own fill goes where all its cells are in the box.
      const across = (tab) => box.left <= 1 && box.right >= tab.columns.length;
      return commit(
        mapCells(
          edit(sheet, (tab) => ({
            ...tab,
            columns: tab.columns.map((column, c) =>
              whole && within(c + 1)
                ? plainly(column, ["wrap"], ["data-format"])
                : column,
            ),
            rows: tab.rows.map((row, r) => {
              const inside = r + 1 >= box.top && r + 1 <= box.bottom;
              const attrs =
                across(tab) && inside && !skip.has(r + 1)
                  ? plainly(row.attrs, [], ["data-fill"])
                  : row.attrs;
              return attrs === row.attrs ? row : { ...row, attrs };
            }),
          })),
          (cell, at, row, column) => {
            const inside = row >= box.top && row <= box.bottom;
            if (at !== sheet || !inside || !within(column) || skip.has(row))
              return cell;
            const attrs = plainly(cell.attrs, looks, [
              "data-fill",
              "data-ink",
              "data-format",
            ]);
            return attrs === cell.attrs ? cell : { ...cell, attrs };
          },
        ),
      );
    }),

    // The tab's table as it would be with text in a cell, cut around that
    // text, and whole with the cell emptied where that ends the table
    // sooner; nothing where the text cannot be kept. No step is made.
    draft(sheet, at) {
      if (locked || tooFar(sheet, at)) return undefined;
      const made = (text) => {
        const far = text ? at : { row: 0, column: 0 };
        const tab = reaching(tabs[sheet], far.row, far.column);
        const row = tab.rows[at.row - 1];
        const cell = row?.cells[at.column - 1];
        if (!cell) return tableText(tab);
        const cells = row.cells.with(at.column - 1, withText(cell, text));
        const rows = tab.rows.with(at.row - 1, { ...row, cells });
        return tableText(trimmed({ ...tab, rows }));
      };
      // No text read from a file holds this character.
      const [head, tail] = made("\0").split("\0");
      const empty = made("");
      return { head, tail, ...(empty !== head + tail && { empty }) };
    },

    // Text set down from a cell on. From this book's own cells (`from`)
    // formulas move along, and a cut clears.
    paste: queued((sheet, at, rows, from) => {
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
    }),

    // One row alone takes the row above it.
    fillDown: queued((sheet, box) => {
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
    }),

    insertRows: moving((sheet, at, count) =>
      restructured(
        (model) => model.insertRows(sheet, at, count),
        edit(sheet, (tab) => ({
          ...tab,
          attrs: slidAttrs(tab, false, at, count),
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
    ),
    deleteRows: moving(
      (sheet, at, count) =>
        count < tabs[sheet].rows.length &&
        restructured(
          (model) => model.deleteRows(sheet, at, count),
          edit(sheet, (tab) => ({
            ...tab,
            attrs: slidAttrs(tab, false, at, -count),
            charts: slidCharts(tab.charts, false, at, -count),
            rows: tab.rows.toSpliced(at - 1, count),
          })),
        ),
    ),
    insertColumns: moving((sheet, at, count) =>
      restructured(
        (model) => model.insertColumns(sheet, at, count),
        edit(sheet, (tab) => ({
          ...tab,
          attrs: slidAttrs(tab, true, at, count),
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
    ),
    deleteColumns: moving(
      (sheet, at, count) =>
        count < tabs[sheet].columns.length &&
        restructured(
          (model) => model.deleteColumns(sheet, at, count),
          edit(sheet, (tab) => ({
            ...tab,
            attrs: slidAttrs(tab, true, at, -count),
            charts: slidCharts(tab.charts, true, at, -count),
            columns: tab.columns.toSpliced(at - 1, count),
            rows: tab.rows.map((row) => ({
              ...row,
              cells: row.cells.toSpliced(at - 1, count),
            })),
          })),
        ),
    ),

    // Rows or columns moved `by` places among their neighbours. Every
    // formula's reference to one of their cells follows, on any tab, a
    // chart its cells, a rule its column and a merged cell, which moves
    // whole. A header row and a total row stay, and no row passes one.
    moveRows: moving((sheet, at, count, by) => {
      const to = at + by;
      const [first, last] = [Math.min(at, to), Math.max(at, to) + count - 1];
      if (to < 1 || last > tabs[sheet].rows.length) return false;
      if (first <= headOf(tabs[sheet])) return "A header row keeps its place.";
      if (tabs[sheet].rows.slice(first - 1, last).some(isTotal))
        return "A total row keeps its place.";
      const rows = lifting(at, count, by);
      const place = ({ row, column }) => ({ row: rows(row), column });
      const merged = carriedMerges(tabs[sheet].attrs, place);
      if (merged.cut) return MOVED;
      if (merged.group) return GROUPED;
      return following(
        sheet,
        place,
        edit(sheet, (tab) => ({
          ...tab,
          attrs: merged.attrs,
          charts: carriedCharts(tab.charts, place),
          rows: lifted(tab.rows, at, count, by),
        })),
      );
    }),
    moveColumns: moving((sheet, at, count, by) => {
      const to = at + by;
      if (to < 1 || Math.max(at, to) + count - 1 > tabs[sheet].columns.length)
        return false;
      const place = lifting(at, count, by);
      const cell = ({ row, column }) => ({ row, column: place(column) });
      const merged = carriedMerges(tabs[sheet].attrs, cell);
      if (merged.cut) return MOVED;
      if (merged.group) return GROUPED;
      return following(
        sheet,
        cell,
        edit(sheet, (tab) => ({
          ...tab,
          charts: carriedCharts(tab.charts, cell),
          attrs: tab.attrs["data-rule"]
            ? {
                ...merged.attrs,
                "data-rule": slidRules(
                  tab.attrs["data-rule"],
                  tab.columns.length,
                  place,
                ),
              }
            : merged.attrs,
          columns: lifted(tab.columns, at, count, by),
          rows: tab.rows.map((row) => ({
            ...row,
            cells: lifted(row.cells, at, count, by),
          })),
        })),
      );
    }),

    // A column's width in pixels; without one it is as wide as its texts.
    widen: queued((sheet, { left, right }, width) =>
      commit(
        edit(sheet, (tab) => ({
          ...tab,
          columns: tab.columns.map((column, c) => {
            const { "data-width": _, ...rest } = column;
            if (c + 1 < left || c + 1 > right) return column;
            return width ? { ...column, "data-width": `${width}` } : rest;
          }),
        })),
      ),
    ),

    // Under the header and but for total rows; a formula follows its row.
    sort: moving((sheet, column, descending) => {
      const tab = tabs[sheet];
      const places = tab.rows
        .map((_, at) => at)
        .filter((at) => at >= headOf(tab) && !isTotal(tab.rows[at]));
      const keyOf = (at) => {
        const { kind, number, shown } = cellAt(sheet, at + 1, column);
        return { kind, value: kind === "number" ? number : shown };
      };
      const sorted = places
        .map((at) => ({
          at,
          key: tab.rows[at].cells[column - 1].text
            ? keyOf(at)
            : { kind: "empty" },
        }))
        .sort((a, b) => inOrder(a.key, b.key, descending) || a.at - b.at);
      // A merged cell goes with its row; one across rows would be cut.
      const moves = new Map(places.map((to, n) => [sorted[n].at + 1, to + 1]));
      const merged = carriedMerges(tab.attrs, ({ row, column }) => ({
        row: moves.get(row) ?? row,
        column,
      }));
      if (merged.cut)
        return `Not sorted: ${merged.cut} is a merged cell across rows.`;
      if (merged.group)
        return `Not sorted: the sort would move rows across the group ${merged.group}.`;
      const rows = [...tab.rows];
      places.forEach((to, n) => {
        const from = sorted[n].at;
        if (from !== to)
          rows[to] = {
            ...tab.rows[from],
            cells: tab.rows[from].cells.map((cell) =>
              rewritten(
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
          : edit(sheet, () => ({ ...tab, attrs: merged.attrs, rows })),
      );
    }),
  };
}
