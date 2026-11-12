// A tab's simple rules: `data-rule="D = Done: strike muted; < 0: bad"`,
// read into what the engine's conditional formatting takes.

import { columnOf, letters, SPARE } from "./text.js";

// A fill's colour in the engine and in a spreadsheet file; the grid draws
// a fill by its name, in the theme's colours.
export const FILLS = {
  accent: "#DCE6F9",
  good: "#DCF5E3",
  warn: "#FDF0C8",
  bad: "#FBDCDC",
  muted: "#F3F3F3",
};
const FONT = { bold: "b", strike: "strike" };

// A value as a formula writes it. `number` for a number, a percentage and a
// date: a blank cell counts as 0 and must not match one.
const valueOf = (said) => {
  const text = said.replace(/^"(.*)"$/, "$1");
  const [, year, month, day] = /^(\d{4})-(\d\d)-(\d\d)$/.exec(text) ?? [];
  if (year)
    return {
      formula: `DATE(${year},${Number(month)},${Number(day)})`,
      number: true,
    };
  if (/^-?\d+(\.\d+)?%?$/.test(text)) return { formula: text, number: true };
  return {
    formula: /^(true|false)$/i.test(text)
      ? text.toUpperCase()
      : `"${text.replaceAll('"', '""')}"`,
  };
};
const numeric = (test) => (ref) => `AND(ISNUMBER(${ref}),${test(ref)})`;
const TESTS = [
  [
    /^(<>|=)\s*(.+)$/,
    (sign, said) => {
      const { formula, number } = valueOf(said);
      const is = (ref) => `${ref}${sign}${formula}`;
      // Nothing is unlike a blank cell: it holds nothing to compare.
      if (sign === "<>") return (ref) => `AND(${ref}<>"",${is(ref)})`;
      return number ? numeric(is) : is;
    },
  ],
  [
    /^(<=|>=|<|>)\s*(.+)$/,
    (sign, said) => {
      const { formula, number } = valueOf(said);
      return number && numeric((ref) => `${ref}${sign}${formula}`);
    },
  ],
  [/^(not )?blank$/, (not) => (ref) => `${ref}${not ? "<>" : "="}""`],
  [
    /^(before|after) today$/,
    (when) => numeric((ref) => `${ref}${when === "before" ? "<" : ">"}TODAY()`),
  ],
];
const testOf = (condition) => {
  for (const [shape, make] of TESTS) {
    const read = shape.exec(condition);
    if (read) return make(...read.slice(1));
  }
};
const formatOf = (effects) => {
  const format = { font: {} };
  for (const word of effects.trim().split(/\s+/)) {
    if (Object.hasOwn(FILLS, word)) format.fill = { color: FILLS[word] };
    else if (Object.hasOwn(FONT, word)) format.font[FONT[word]] = true;
    else return undefined;
  }
  return format;
};
// A table's rule starts with the letter of a column of the grid: one the
// tab has, or a spare one past it, whose cells are blank.
const columnIn = (rule, width) => {
  const [, name, rest] = /^\s*([A-Za-z]+)(\s.*)$/s.exec(rule) ?? [];
  const column = name ? columnOf(name) : 0;
  return column >= 1 && column <= width + SPARE.columns
    ? { column, rest }
    : undefined;
};

// On a <col>, `own` is its column and a rule styles the cell it tests; on
// the table a rule styles the row. `unread` are those not understood.
export function readRules(said = "", width, own) {
  const rules = [];
  const unread = [];
  for (const text of said.split(";").map((rule) => rule.trim())) {
    const named = own ? { column: own, rest: text } : columnIn(text, width);
    const [, condition, effects] = /^(.*):([^:]*)$/.exec(named?.rest) ?? [];
    const test = effects !== undefined && testOf(condition.trim());
    const format = test && formatOf(effects);
    if (format)
      rules.push({
        text,
        column: named.column,
        row: !own,
        test,
        format,
        condition: condition.trim(),
        effects: effects.trim().split(/\s+/),
      });
    else if (text) unread.push(text);
  }
  return { rules, unread };
}

const each = (said = "") =>
  said
    .split(";")
    .map((rule) => rule.trim())
    .filter(Boolean);
const without = ({ "data-rule": _, ...attrs }) => attrs;
const ruled = (attrs, rules) =>
  rules.length ? { ...attrs, "data-rule": rules.join("; ") } : without(attrs);

// A column's rules as a person sets them: the table's that test it, on the
// row and said without its letter, then its own, on its cell. One that is
// understood says its condition and its effects.
export function rulesAt(tab, column) {
  const width = tab.columns.length;
  const said = (text, row) => {
    const [rule] = readRules(text, width, !row && column).rules;
    return {
      text: row ? columnIn(text, width).rest.trim() : text,
      row,
      condition: rule?.condition,
      effects: rule?.effects,
    };
  };
  return [
    ...each(tab.attrs["data-rule"])
      .filter((text) => columnIn(text, width)?.column === column)
      .map((text) => said(text, true)),
    ...each(tab.columns[column - 1]?.["data-rule"]).map((text) =>
      said(text, false),
    ),
  ];
}

// The tab with the rule `rulesAt` lists at `at` set to `rule`, which is
// `{ text, row }`, or taken out without one. It keeps its place where it
// stays on the row or on the cell; else, and past the last, it is one more.
export function withRule(tab, column, at, rule) {
  const width = tab.columns.length;
  const now = rulesAt(tab, column).map((was, place) =>
    place !== at ? was : rule?.row === was.row ? rule : undefined,
  );
  if (rule && !now.includes(rule)) now.push(rule);
  const lettered = ({ text }) => `${letters(column)} ${text}`;
  let place = 0;
  const table = each(tab.attrs["data-rule"]).flatMap((text) => {
    if (columnIn(text, width)?.column !== column) return [text];
    const set = now[place++];
    return !set ? [] : set === rule ? [lettered(set)] : [text];
  });
  const rest = now.slice(place).filter(Boolean);
  const on = (row) => rest.filter((set) => set.row === row);
  return {
    ...tab,
    attrs: ruled(tab.attrs, [...table, ...on(true).map(lettered)]),
    columns: tab.columns.map((attrs, c) =>
      c === column - 1
        ? ruled(
            attrs,
            on(false).map(({ text }) => text),
          )
        : attrs,
    ),
  };
}

// The rule's formula reads the first of the rows `top` to `bottom`.
export const ruleInput = (
  { column, row, test, format },
  top,
  bottom,
  width,
) => ({
  range: `${letters(row ? 1 : column)}${top}:${letters(row ? width : column)}${bottom}`,
  rule: {
    type: "Formula",
    formula: test(`${row ? "$" : ""}${letters(column)}${top}`),
    format,
    stop_if_true: false,
  },
});

// A table's rules after its columns moved. One that names no column of the
// grid stays as written; one whose column is gone goes with it.
export const slidRules = (said, width, slide) =>
  said
    .split(";")
    .flatMap((rule) => {
      const named = columnIn(rule, width);
      const column = named && slide(named.column);
      if (!named) return rule.trim() ? [rule.trim()] : [];
      return column ? [`${letters(column)}${named.rest.trimEnd()}`] : [];
    })
    .join("; ");
