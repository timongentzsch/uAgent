import test from "node:test";
import assert from "node:assert/strict";
import { readRules, ruleInput, slidRules } from "../sheet/rules.js";

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
