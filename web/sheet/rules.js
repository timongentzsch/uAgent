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
      rules.push({ text, column: named.column, row: !own, test, format });
    else if (text) unread.push(text);
  }
  return { rules, unread };
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
