import test from "node:test";
import assert from "node:assert/strict";
import {
  readRules,
  ruleInput,
  rulesAt,
  slidRules,
  withRule,
} from "../sheet/rules.js";

// The formula a condition makes for the cell B2.
const formula = (condition) =>
  readRules(`${condition}: bold`, 4, 2).rules[0]?.test("B2");

test("each condition reads as the formula the engine tests", () => {
  assert.equal(formula("= Done"), 'B2="Done"');
  assert.equal(formula('= "Done"'), 'B2="Done"');
  assert.equal(formula('= say "hi"'), 'B2="say ""hi"""');
  // A number is compared as one, and never with a blank cell, which is 0.
  assert.equal(formula("= 0"), "AND(ISNUMBER(B2),B2=0)");
  assert.equal(formula("<= -2.5"), "AND(ISNUMBER(B2),B2<=-2.5)");
  assert.equal(formula("> 1000"), "AND(ISNUMBER(B2),B2>1000)");
  assert.equal(formula("blank"), 'B2=""');
  assert.equal(formula("not blank"), 'B2<>""');
  assert.equal(formula("before today"), "AND(ISNUMBER(B2),B2<TODAY())");
  assert.equal(formula("after today"), "AND(ISNUMBER(B2),B2>TODAY())");
});

test("TRUE, a date and a percentage compare as what they are, not as text", () => {
  assert.equal(formula("= TRUE"), "B2=TRUE");
  assert.equal(formula("= false"), "B2=FALSE");
  assert.equal(formula("= 2027-03-01"), "AND(ISNUMBER(B2),B2=DATE(2027,3,1))");
  assert.equal(formula("< 2027-03-01"), "AND(ISNUMBER(B2),B2<DATE(2027,3,1))");
  assert.equal(formula("= 4.5%"), "AND(ISNUMBER(B2),B2=4.5%)");
  assert.equal(formula(">= 10%"), "AND(ISNUMBER(B2),B2>=10%)");
  // Nothing is unlike a blank cell.
  assert.equal(formula("<> Done"), 'AND(B2<>"",B2<>"Done")');
  assert.equal(formula("<> 5"), 'AND(B2<>"",B2<>5)');
  assert.equal(formula("<> TRUE"), 'AND(B2<>"",B2<>TRUE)');
  // More or less than a text is no test.
  assert.equal(formula("< Done"), undefined);
});

test("a column's rule styles its cell, a table's the row of the column it names", () => {
  const { rules, unread } = readRules("< 0: bad; > 1000: bold strike", 6, 4);
  assert.deepEqual(unread, []);
  assert.deepEqual(
    rules.map(({ format }) => format),
    [
      { font: {}, fill: { color: "#FBDCDC" } },
      { font: { b: true, strike: true } },
    ],
  );
  assert.deepEqual(ruleInput(rules[0], 2, 9, 6), {
    range: "D2:D9",
    rule: {
      type: "Formula",
      formula: "AND(ISNUMBER(D2),D2<0)",
      format: rules[0].format,
      stop_if_true: false,
    },
  });
  const [row] = readRules("D = Done: strike muted", 6).rules;
  const input = ruleInput(row, 2, 9, 6);
  assert.equal(input.range, "A2:F9");
  assert.equal(input.rule.formula, '$D2="Done"');
});

test("a rule that is not understood is set apart, and the others stand", () => {
  const { rules, unread } = readRules(
    "D = Done: strike; D between 1 and 2: bad; D > 0: blue; = x: bold; E blank: warn; D < 0: constructor",
    5,
  );
  assert.deepEqual(
    rules.map(({ text }) => text),
    ["D = Done: strike", "E blank: warn"],
  );
  assert.deepEqual(unread, [
    "D between 1 and 2: bad",
    "D > 0: blue",
    "= x: bold",
    "D < 0: constructor",
  ]);
  assert.deepEqual(readRules("", 3, 1), { rules: [], unread: [] });
  assert.deepEqual(readRules(undefined, 3), { rules: [], unread: [] });
});

test("a table's rule names a column of the grid: one the tab has or a spare one past it; a word is none", () => {
  const said =
    "Status = Done: strike; Tax > 0: bad; not blank: muted; Z blank: muted; B blank: muted";
  const { rules, unread } = readRules(said, 4);
  assert.deepEqual(
    rules.map(({ text }) => text),
    ["Z blank: muted", "B blank: muted"],
  );
  assert.equal(unread.length, 3);
  // Columns put in or taken out move only the rules that name one.
  assert.equal(
    slidRules(said, 4, (column) => column + 1),
    "Status = Done: strike; Tax > 0: bad; not blank: muted; AA blank: muted; C blank: muted",
  );
});

test("a table's rules follow their columns, and go with a column taken out", () => {
  const said = "D = Done: strike muted; B before today: bad";
  // A column put in before C; then column D taken out.
  assert.equal(
    slidRules(said, 4, (column) => (column < 3 ? column : column + 1)),
    "E = Done: strike muted; B before today: bad",
  );
  assert.equal(
    slidRules(said, 4, (column) => (column === 4 ? undefined : column)),
    "B before today: bad",
  );
});

// A tab of three columns: the table's rules, and column C's own.
const ruled = (table, column) => ({
  attrs: table === undefined ? {} : { "data-frozen": "1", "data-rule": table },
  columns: [{}, {}, column === undefined ? {} : { "data-rule": column }],
});

test("a column's rules are the table's that test it, then its own, each as it is understood", () => {
  const tab = ruled(
    "C = Done: strike muted; B blank: warn; C glows: bad; nothing",
    '> 1000: bold; = "a: b": good bad',
  );
  assert.deepEqual(rulesAt(tab, 3), [
    {
      text: "= Done: strike muted",
      row: true,
      condition: "= Done",
      effects: ["strike", "muted"],
    },
    { text: "glows: bad", row: true, condition: undefined, effects: undefined },
    { text: "nothing", row: true, condition: undefined, effects: undefined },
    {
      text: "> 1000: bold",
      row: false,
      condition: "> 1000",
      effects: ["bold"],
    },
    {
      text: '= "a: b": good bad',
      row: false,
      condition: '= "a: b"',
      effects: ["good", "bad"],
    },
  ]);
  // One that names no column of the grid is listed with every column's.
  assert.deepEqual(
    rulesAt(tab, 2).map(({ text }) => text),
    ["blank: warn", "nothing"],
  );
  assert.deepEqual(rulesAt(ruled("Tax > 0: bad; BL > 0: bad"), 1), [
    {
      text: "Tax > 0: bad",
      row: true,
      condition: undefined,
      effects: undefined,
    },
    {
      text: "BL > 0: bad",
      row: true,
      condition: undefined,
      effects: undefined,
    },
  ]);
  // A spare column past the table has the table's rules for it, none of its own.
  assert.deepEqual(rulesAt(ruled("E > 0: bad"), 5), [
    { text: "> 0: bad", row: true, condition: "> 0", effects: ["bad"] },
  ]);
  assert.deepEqual(rulesAt(ruled(), 1), []);
});

test("one of a column's rules is set in its place, taken out, or added: the others stand as written", () => {
  const tab = ruled(
    "c  = Done: strike; B blank: warn; C < 0: bad",
    "> 1: bold",
  );
  const set = (at, rule, of = tab) => {
    const { attrs, columns } = withRule(of, 3, at, rule);
    return [attrs["data-rule"], columns[2]["data-rule"]];
  };
  // Changed where it stood, before the other column's.
  assert.deepEqual(set(0, { text: "= Open: good", row: true }), [
    "C = Open: good; B blank: warn; C < 0: bad",
    "> 1: bold",
  ]);
  // Taken out: the one after it stays after the other column's.
  assert.deepEqual(set(0), ["B blank: warn; C < 0: bad", "> 1: bold"]);
  assert.deepEqual(set(2), [
    "c  = Done: strike; B blank: warn; C < 0: bad",
    undefined,
  ]);
  // One more, on the row or on the cell, follows the last.
  assert.deepEqual(set(3, { text: "blank: muted", row: true }), [
    "c  = Done: strike; B blank: warn; C < 0: bad; C blank: muted",
    "> 1: bold",
  ]);
  assert.deepEqual(
    set(3, { text: "> 9: bad", row: false })[1],
    "> 1: bold; > 9: bad",
  );
  // Moved from the row to the cell, and from the cell to the row.
  assert.deepEqual(set(1, { text: "< 0: bad", row: false }), [
    "c  = Done: strike; B blank: warn",
    "> 1: bold; < 0: bad",
  ]);
  assert.deepEqual(set(2, { text: "> 1: bold", row: true }), [
    "c  = Done: strike; B blank: warn; C < 0: bad; C > 1: bold",
    undefined,
  ]);
  // One listed with every column's is taken out from any; and the places
  // are those of the table as wide as it was before it reached the column.
  assert.deepEqual(
    withRule(ruled("nothing; C blank: bad"), 3, 0).attrs["data-rule"],
    "C blank: bad",
  );
  const far = {
    ...ruled("AE = 1: bad; AF = 1: bad"),
    columns: Array(32).fill({}),
  };
  assert.deepEqual(
    withRule(far, 32, 1, undefined, 3).attrs["data-rule"],
    "AE = 1: bad",
  );
  // With none left the attribute goes, and the others stay as they were.
  const bare = withRule(ruled("C blank: bad"), 3, 0);
  assert.deepEqual(bare.attrs, { "data-frozen": "1" });
  assert.deepEqual(set(0, { text: "> 0: bad", row: true }, ruled()), [
    "C > 0: bad",
    undefined,
  ]);
});
