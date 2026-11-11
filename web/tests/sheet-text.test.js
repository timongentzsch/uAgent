// What a sheet page works out without the page or the engine (sheet/text.js).
import test from "node:test";
import assert from "node:assert/strict";
import {
  boxName,
  boxOf,
  cellName,
  edgeFrom,
  fromTsv,
  frozenOf,
  inOrder,
  letters,
  moved,
  tableText,
  toTsv,
} from "../sheet/text.js";

test("cells and ranges are named as a spreadsheet names them", () => {
  assert.deepEqual([1, 26, 27, 52, 703].map(letters), [
    "A",
    "Z",
    "AA",
    "AZ",
    "AAA",
  ]);
  assert.equal(cellName({ row: 12, column: 28 }), "AB12");
  const box = boxOf({ row: 6, column: 3 }, { row: 2, column: 1 });
  assert.deepEqual(box, { top: 2, left: 1, bottom: 6, right: 3 });
  assert.equal(boxName(box), "A2:C6");
  assert.equal(
    boxName(boxOf({ row: 2, column: 2 }, { row: 2, column: 2 })),
    "B2",
  );
});

test("a tab holds in view what it says, else its header rows", () => {
  assert.deepEqual(frozenOf("2 B", 5), { rows: 2, columns: 2 });
  assert.deepEqual(frozenOf("A"), { rows: 0, columns: 1 });
  assert.deepEqual(frozenOf(undefined, 3), { rows: 3, columns: 0 });
});

test("Ctrl and an arrow run along what is filled, then jump to the next filled cell or the edge", () => {
  // Row 1 of a tab six columns wide: A, B and C filled, D empty, E filled.
  const filled = ({ column }) => [1, 2, 3, 5].includes(column);
  const size = { rows: 3, columns: 6 };
  const right = (column) =>
    edgeFrom(filled, size, { row: 1, column }, { row: 0, column: 1 }).column;
  assert.deepEqual([1, 2, 3, 4, 5, 6].map(right), [3, 3, 5, 5, 6, 6]);
  const left = (column) =>
    edgeFrom(filled, size, { row: 1, column }, { row: 0, column: -1 }).column;
  assert.deepEqual([6, 5, 4, 3, 1].map(left), [5, 3, 3, 1, 1]);
});

test("tab-separated text is written and read with its quoted cells", () => {
  const rows = [
    ["a", "", "two\nlines"],
    ['say "hi"', "tab\there", "=A1*2"],
  ];
  const text = 'a\t\t"two\nlines"\n"say ""hi"""\t"tab\there"\t=A1*2';
  assert.equal(toTsv(rows), text);
  assert.deepEqual(fromTsv(text), rows);
  // As another spreadsheet ends it, and with an empty last cell.
  assert.deepEqual(fromTsv("1\t2\r\n3\t\r\n"), [
    ["1", "2"],
    ["3", ""],
  ]);
  assert.deepEqual(fromTsv('5" wide'), [['5" wide']]);
  assert.deepEqual(fromTsv(""), [[""]]);
});

// Where the references of a formula are, as the engine's tokens say.
const spans = (formula, ...refs) => {
  let from = 0;
  return refs.map((ref) => {
    const start = [...formula.slice(0, formula.indexOf(ref, from))].length;
    from = formula.indexOf(ref, from) + ref.length;
    return { start, end: start + [...ref].length };
  });
};

test("a formula moved takes its relative references along", () => {
  const formula = "=B2*$C2+D$4+SUM($E$1:F3)+Rates!A2";
  const at = spans(formula, "B2", "$C2", "D$4", "$E$1:F3", "Rates!A2");
  assert.equal(
    moved(formula, at, { row: 2, column: 1 }),
    "=C4*$C4+E$4+SUM($E$1:G5)+Rates!B4",
  );
  // Past the sheet's edge a reference is lost.
  assert.equal(
    moved("=A1+B2", spans("=A1+B2", "A1", "B2"), { row: -1, column: 0 }),
    "=#REF!+B1",
  );
  // Whole columns and rows, and text before the references of any width.
  assert.equal(
    moved(
      '="é😀"&SUM(A:A)+SUM(2:3)',
      spans('="é😀"&SUM(A:A)+SUM(2:3)', "A:A", "2:3"),
      {
        row: 1,
        column: 1,
      },
    ),
    '="é😀"&SUM(B:B)+SUM(3:4)',
  );
});

test("a sorted row's formula follows its own row and keeps every other reference", () => {
  const formula = "=B4*2+B5+SUM($A4:C4)+'Tasks'!D4+Rates!B4";
  const at = spans(formula, "B4", "B5", "$A4:C4", "'Tasks'!D4", "Rates!B4");
  const own = { row: 4, tab: "Tasks" };
  assert.equal(
    moved(formula, at, { row: -2, column: 0 }, own),
    "=B2*2+B5+SUM($A2:C2)+'Tasks'!D2+Rates!B4",
  );
  // A range that lies partly on the row has no place after the sort, and a
  // reference of another kind is not read: neither is guessed at.
  for (const [stuck, ...refs] of [
    ["=SUM(B2:B4)", "B2:B4"],
    ["=SUM(Costs[Net])", "Costs[Net]"],
    // Written with spaces, a range comes as its two ends.
    ["=SUM( B4 : B6 )", " B4", " B6"],
  ])
    assert.equal(
      moved(stuck, spans(stuck, ...refs), { row: -2, column: 0 }, own),
      null,
    );
  // A fill moves both ends of such a range.
  assert.equal(
    moved("=SUM( B4 : B6 )", spans("=SUM( B4 : B6 )", " B4", " B6"), {
      row: 1,
      column: 0,
    }),
    "=SUM( B5 : B7 )",
  );
  assert.equal(
    moved(
      "=SUM(B2:B3)",
      spans("=SUM(B2:B3)", "B2:B3"),
      { row: 1, column: 0 },
      own,
    ),
    "=SUM(B2:B3)",
  );
});

test("a column sorts numbers, text, booleans and errors apart, and what is empty last", () => {
  const keys = [
    { kind: "text", value: "pear" },
    { kind: "empty" },
    { kind: "number", value: 10 },
    { kind: "error", value: "#DIV/0!" },
    { kind: "text", value: "Apple" },
    { kind: "number", value: 9 },
    { kind: "boolean", value: "TRUE" },
    { kind: "text", value: "item 10" },
    { kind: "text", value: "item 9" },
  ];
  const sorted = (descending) =>
    [...keys]
      .sort((a, b) => inOrder(a, b, descending))
      .map((key) => key.value ?? "");
  assert.deepEqual(sorted(false), [
    9,
    10,
    "Apple",
    "item 9",
    "item 10",
    "pear",
    "TRUE",
    "#DIV/0!",
    "",
  ]);
  assert.deepEqual(sorted(true), [
    "#DIV/0!",
    "TRUE",
    "pear",
    "item 10",
    "item 9",
    "Apple",
    10,
    9,
    "",
  ]);
});

const cell = (text, attrs = {}, th = false) => ({ th, attrs, text });

test("a tab is written as the table the skill teaches, with every mark it lists", () => {
  const tab = {
    name: "Tasks",
    attrs: { "data-frozen": "1 A" },
    columns: [
      { "data-width": "240", class: "wrap" },
      { "data-options": "To do|In progress|Done" },
      { "data-control": "checkbox" },
      { "data-format": "#,##0.00" },
    ],
    rows: [
      {
        attrs: {},
        cells: ["Task", "Status", "Done", "Cost"].map((text) =>
          cell(text, {}, true),
        ),
      },
      {
        attrs: { "data-fill": "muted" },
        cells: [
          cell("Write <the> plan & more"),
          cell("Done", { "data-fill": "good" }),
          cell("TRUE"),
          cell("1200"),
        ],
      },
      {
        attrs: {},
        cells: [
          cell("Rate", { class: "wrap" }),
          cell(""),
          cell(""),
          cell("4.5%", { "data-format": "0.0%" }),
        ],
      },
      {
        attrs: { class: "total" },
        cells: [
          cell("Total"),
          cell(""),
          cell("=COUNTIF(C2:C3,TRUE)"),
          cell("=SUM(D2:D2)"),
        ],
      },
    ],
  };
  assert.equal(
    tableText(tab),
    [
      '<table data-sheet="Tasks" data-frozen="1 A">',
      '<col data-width="240" class="wrap"><col data-options="To do|In progress|Done"><col data-control="checkbox"><col data-format="#,##0.00">',
      "<tr><th>Task<th>Status<th>Done<th>Cost",
      '<tr data-fill="muted"><td>Write &lt;the&gt; plan &amp; more<td data-fill="good">Done<td>TRUE<td>1200',
      '<tr><td class="wrap">Rate<td><td><td data-format="0.0%">4.5%',
      '<tr class="total"><td>Total<td><td>=COUNTIF(C2:C3,TRUE)<td>=SUM(D2:D2)',
      "</table>",
    ].join("\n"),
  );
  // No column says anything: no line of them. A name is an attribute's text.
  assert.equal(
    tableText({
      name: 'A "b" & c',
      attrs: {},
      columns: [{}, {}],
      rows: [{ attrs: {}, cells: [cell("1"), cell("")] }],
    }),
    '<table data-sheet="A &quot;b&quot; &amp; c">\n<tr><td>1<td>\n</table>',
  );
});
