// A sheet's tabs and the engine that calculates them (sheet/book.js), with
// the engine itself: what an edit, a moved row or a sort leaves in the file.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { headOf, openBook } from "../sheet/book.js";

const wasm = createRequire(import.meta.url).resolve("@ironcalc/wasm-xlsx");
const engine = await import(wasm);
engine.initSync({
  module: readFileSync(wasm.replace(/wasm\.js$/, "wasm_bg.wasm")),
});

// A tab from rows of what a person typed: "#" before a text makes a header
// cell, a leading class in braces marks the row.
const tab = (name, written, columns = []) => {
  const rows = written.map((row) => {
    const marked = /^\{(\w+)\}$/.exec(row[0])?.[1];
    return {
      attrs: marked ? { class: marked } : {},
      texts: marked ? row.slice(1) : row,
    };
  });
  const width = Math.max(...rows.map((row) => row.texts.length));
  return {
    name,
    attrs: {},
    markup: [],
    charts: [],
    columns: Array.from({ length: width }, (_, at) => columns[at] ?? {}),
    rows: rows.map(({ attrs, texts }) => ({
      attrs,
      cells: Array.from({ length: width }, (_, at) => ({
        th: !!texts[at]?.startsWith("#"),
        attrs: {},
        text: (texts[at] ?? "").replace(/^#/, ""),
      })),
    })),
  };
};
const plan = () =>
  openBook(engine, [
    tab(
      "Tasks",
      [
        ["Done", "=COUNTIF(B4:B6,A1)", ""],
        ["", "", ""],
        ["#Task", "#Status", "#Cost"],
        ["Write", "Done", "1200"],
        ["Book", "Done", "8400.5"],
        ["Brief", "To do", "=C4/10"],
        ["{total}", "Total", "", "=SUM(C4:C6)"],
      ],
      [{}, {}, { "data-format": "#,##0.00" }],
    ),
    tab("Inputs", [
      ["#Name", "#Value"],
      ["Budget", "=Tasks!C7*2"],
      ["Rate", "4.5%"],
    ]),
  ]);
const texts = (book, sheet) =>
  book.tabs[sheet].rows.map((row) => row.cells.map((cell) => cell.text));
const shown = (book, sheet, row, columns) =>
  Array.from({ length: columns }, (_, at) => book.shown(sheet, row, at + 1));

test("the engine says what cells show; what was typed stays as typed", () => {
  const book = plan();
  assert.equal(book.shown(0, 1, 2), "2");
  assert.deepEqual(shown(book, 0, 7, 3), ["Total", "", "9,720.50"]);
  assert.equal(book.shown(1, 2, 2), "19,441.00");
  assert.equal(book.shown(1, 3, 2), "4.50%");
  assert.equal(book.tabs[1].rows[2].cells[1].text, "4.5%");
  assert.equal(book.kind(0, 4, 3), "number");
  assert.equal(headOf(book.tabs[0]), 3);
  assert.deepEqual(book.errors(), []);
});

test("a formula shows in its own column's pattern, and without one as the cells it reads do", () => {
  const book = openBook(engine, [
    tab(
      "Costs",
      [
        ["=B1*2", "1200", "=B1/B2", "=B1*2"],
        ["", "2400", "", ""],
      ],
      [{}, { "data-format": "#,##0.00" }, { "data-format": "0%" }],
    ),
  ]);
  assert.deepEqual(shown(book, 0, 1, 4), [
    "2,400.00",
    "1,200.00",
    "50%",
    "2,400.00",
  ]);
});

test("what a typed value brings stands over its column's pattern: a percentage, a date, a currency", () => {
  const book = openBook(engine, [
    tab(
      "Inputs",
      [["4.5%", "2027-03-01", "$1,200", "1200", "=A1*2", "=D1*2"]],
      Array.from({ length: 6 }, () => ({ "data-format": "#,##0.00" })),
    ),
  ]);
  assert.deepEqual(shown(book, 0, 1, 6), [
    "4.50%",
    "2027-03-01",
    "$1,200",
    "1,200.00",
    "0.09",
    "2,400.00",
  ]);
});

test("a typed cell is one step: dependents follow, undo and redo take it back and forth", () => {
  const book = plan();
  assert.equal(book.canUndo(), false);
  assert.equal(book.paste(0, { row: 4, column: 3 }, [["2000"]]), true);
  assert.equal(book.shown(0, 7, 3), "10,600.50");
  assert.equal(book.shown(1, 2, 2), "21,201.00");
  // The same text again is no step; nor is nothing set past the table.
  assert.equal(book.paste(0, { row: 4, column: 3 }, [["2000"]]), false);
  assert.equal(book.paste(0, { row: 99, column: 1 }, [[""]]), false);
  assert.equal(book.undo(), true);
  assert.equal(book.shown(0, 7, 3), "9,720.50");
  assert.equal(book.canUndo(), false);
  assert.equal(book.redo(), true);
  assert.equal(book.tabs[0].rows[3].cells[2].text, "2000");
  assert.equal(book.redo(), false);
  assert.equal(book.paste(0, { row: 6, column: 3 }, [["=1/0"]]), true);
  assert.deepEqual(book.errors(), [
    "Tasks!C6 =1/0 is #DIV/0!",
    "Tasks!C7 =SUM(C4:C6) is #DIV/0!",
    "Inputs!B2 =Tasks!C7*2 is #DIV/0!",
  ]);
});

test("a row put in or taken out moves the formulas under it and on other tabs", () => {
  const book = plan();
  book.insertRows(0, 5, 1);
  assert.deepEqual(texts(book, 0).slice(4), [
    ["", "", ""],
    ["Book", "Done", "8400.5"],
    ["Brief", "To do", "=C4/10"],
    ["Total", "", "=SUM(C4:C7)"],
  ]);
  assert.equal(book.tabs[0].rows[7].attrs.class, "total");
  assert.equal(texts(book, 0)[0][1], "=COUNTIF(B4:B7,A1)");
  assert.equal(texts(book, 1)[1][1], "=Tasks!C8*2");
  assert.equal(book.shown(1, 2, 2), "19,441.00");
  book.deleteRows(0, 5, 2);
  assert.deepEqual(texts(book, 0).slice(3), [
    ["Write", "Done", "1200"],
    ["Brief", "To do", "=C4/10"],
    ["Total", "", "=SUM(C4:C5)"],
  ]);
  assert.equal(texts(book, 1)[1][1], "=Tasks!C6*2");
  // Every row cannot go.
  assert.equal(book.deleteRows(1, 1, 3), false);
  book.undo();
  book.undo();
  assert.deepEqual(texts(book, 0), texts(plan(), 0));
});

test("a column put in keeps a header row a header, and moves formulas and the columns' marks", () => {
  const book = plan();
  book.insertColumns(0, 3, 1);
  assert.deepEqual(book.tabs[0].columns, [
    {},
    {},
    {},
    { "data-format": "#,##0.00" },
  ]);
  assert.deepEqual(
    book.tabs[0].rows[2].cells.map((cell) => cell.th),
    [true, true, true, true],
  );
  assert.equal(book.tabs[0].rows[3].cells[2].th, false);
  assert.equal(texts(book, 0)[6][3], "=SUM(D4:D6)");
  assert.equal(texts(book, 1)[1][1], "=Tasks!D7*2");
  book.deleteColumns(0, 1, 1);
  assert.equal(texts(book, 0)[0][0], "=COUNTIF(A4:A6,#REF!)");
  assert.equal(book.deleteColumns(1, 1, 2), false);
});

test("filling down copies what was typed and moves formulas along", () => {
  const book = plan();
  book.paste(0, { row: 4, column: 3 }, [["=B$3&A4"]]);
  book.fillDown(0, { top: 4, left: 2, bottom: 6, right: 3 });
  assert.deepEqual(texts(book, 0).slice(3, 6), [
    ["Write", "Done", "=B$3&A4"],
    ["Book", "Done", "=B$3&A5"],
    ["Brief", "Done", "=B$3&A6"],
  ]);
  // One row takes the row above it.
  book.fillDown(0, { top: 7, left: 1, bottom: 7, right: 1 });
  assert.equal(texts(book, 0)[6][0], "Brief");
});

test("a sort orders the rows under the header, leaves total rows, and keeps each formula on its row", () => {
  const book = plan();
  book.paste(0, { row: 4, column: 3 }, [["=LEN(A4)*1000"], [""]]);
  book.sort(0, 3, false);
  assert.deepEqual(texts(book, 0), [
    ["Done", "=COUNTIF(B4:B6,A1)", ""],
    ["", "", ""],
    ["Task", "Status", "Cost"],
    ["Brief", "To do", "=C4/10"],
    ["Write", "Done", "=LEN(A5)*1000"],
    ["Book", "Done", ""],
    ["Total", "", "=SUM(C4:C6)"],
  ]);
  // "=C4/10" named another row: it stays as written, as a spreadsheet's.
  book.sort(0, 1, true);
  assert.deepEqual(
    texts(book, 0)
      .slice(3, 6)
      .map((row) => row[0]),
    ["Write", "Brief", "Book"],
  );
  assert.equal(texts(book, 0)[3][2], "=LEN(A4)*1000");
  assert.equal(book.sort(0, 1, true), false);
});

test("a sort that would split a formula's range is refused, with why, and nothing moves", () => {
  const book = plan();
  book.paste(0, { row: 4, column: 3 }, [["=SUM(C4:C5)"]]);
  const before = texts(book, 0);
  assert.equal(
    book.sort(0, 1, false),
    "Not sorted: =SUM(C4:C5) cannot be moved with its cell.",
  );
  assert.deepEqual(texts(book, 0), before);
  // The next change is made as any other.
  assert.equal(
    book.fillDown(0, { top: 5, left: 1, bottom: 6, right: 1 }),
    true,
  );
});

test("a row put in leaves a formula it does not move exactly as typed", () => {
  const typed = ["=1e3", "=MYFUNC(A1)", "=A1 B1", "=SUM(A5:A6)"];
  const book = openBook(engine, [
    tab("Odd", [...typed.map((text) => [text]), ["1"], ["2"]]),
  ]);
  book.insertRows(0, 6, 1);
  assert.deepEqual(
    texts(book, 0).map(([text]) => text),
    [...typed.slice(0, 3), "=SUM(A5:A7)", "1", "", "2"],
  );
});

test("tabs may bear the names the engine gives its own sheets", () => {
  const book = openBook(engine, [
    tab("Sheet2", [["=Sheet1!A1+1"]]),
    tab("Sheet1", [["41"]]),
  ]);
  assert.equal(book.shown(0, 1, 1), "42");
  // Nor does a tab named as the sheet the book works on is.
  const odd = openBook(engine, [tab("⌁", [["1", "2"]])]);
  assert.equal(odd.summary(0, { top: 1, left: 1, bottom: 1, right: 2 }).sum, 3);
  assert.deepEqual(
    engine.Model.fromXlsx(odd.xlsx(), "odd", "en", "UTC", "en")
      .getWorksheetsProperties()
      .map((sheet) => sheet.name),
    ["⌁"],
  );
});

test("numbers of twelve digits sort and sum exactly", () => {
  const book = openBook(engine, [
    tab("Big", [["#N"], ["100000000002"], ["100000000001"], ["100000000003"]]),
  ]);
  assert.equal(
    book.summary(0, { top: 2, left: 1, bottom: 4, right: 1 }).sum,
    300000000006,
  );
  book.sort(0, 1, false);
  assert.deepEqual(texts(book, 0).flat(), [
    "N",
    "100000000001",
    "100000000002",
    "100000000003",
  ]);
});

test("a tab with markup in a cell says so with the cells in error, and no change to it is made", () => {
  const marked = {
    ...tab("Costs", [
      ["1", "=IF(A1"],
      ["2", "x"],
    ]),
    markup: ["B1"],
  };
  const book = openBook(engine, [marked, tab("Other", [["=Costs!A2"]])]);
  const why = "Costs!B1 holds markup: write < as &lt;";
  assert.equal(book.errors()[0], why);
  assert.equal(book.paste(0, { row: 2, column: 2 }, [["y"]]), why);
  assert.equal(book.insertRows(0, 1, 1), why);
  // The engine is as it was: the other tab still reads the same cell.
  assert.equal(book.shown(1, 1, 1), "2");
  assert.equal(book.canUndo(), false);
  assert.equal(book.paste(1, { row: 1, column: 1 }, [["7"]]), true);
});

test("undo keeps the last hundred steps and lets go of the oldest", () => {
  const book = plan();
  for (let step = 0; step <= 100; step++)
    assert.equal(book.paste(0, { row: 2, column: 1 }, [[`${step}`]]), true);
  let undone = 0;
  while (book.undo()) undone++;
  assert.equal(undone, 100);
  assert.equal(texts(book, 0)[1][0], "0");
});

test("text set past the table makes it reach there, a header row staying one; blank rows and columns at its end are never kept", () => {
  const book = openBook(engine, [
    tab(
      "Costs",
      [
        ["#Item", "#Net"],
        ["Rent", "1200"],
      ],
      [{}, { "data-rule": "> 1000: bold" }],
    ),
  ]);
  // A column's rule covers what is typed under the table's last row.
  book.paste(0, { row: 3, column: 2 }, [["2000"]]);
  assert.equal(book.look(0, 3, 2).bold, true);
  book.undo();
  const before = book.table(0);
  assert.equal(book.paste(0, { row: 4, column: 4 }, [["=B2*2"]]), true);
  assert.deepEqual(texts(book, 0), [
    ["Item", "Net", "", ""],
    ["Rent", "1200", "", ""],
    ["", "", "", ""],
    ["", "", "", "=B2*2"],
  ]);
  assert.equal(book.shown(0, 4, 4), "2400");
  assert.deepEqual(
    book.tabs[0].rows.map((row) => row.cells.every((cell) => cell.th)),
    [true, false, false, false],
  );
  // Undo takes the reach back; cleared, the table is the used range again.
  book.undo();
  assert.equal(book.table(0), before);
  book.redo();
  assert.equal(book.paste(0, { row: 4, column: 4 }, [[""]]), true);
  assert.equal(book.table(0), before);
  // Filled down past its end, it reaches as far as what is filled.
  book.fillDown(0, { top: 2, left: 1, bottom: 4, right: 3 });
  assert.deepEqual(texts(book, 0).slice(2), [
    ["Rent", "1200"],
    ["Rent", "1200"],
  ]);
  // A blank row put in at its end is no change, and no step.
  const steps = () => {
    let count = 0;
    while (book.undo()) count++;
    return count;
  };
  assert.equal(book.insertRows(0, 5, 2), false);
  assert.equal(steps(), 3);
});

test("pasted text lands from a cell on; this book's own cells bring their formulas, and a cut clears", () => {
  const book = plan();
  book.paste(0, { row: 5, column: 2 }, [
    ["To do", "7"],
    ["Done", "=C5*2"],
    ["beyond", "the"],
  ]);
  assert.deepEqual(texts(book, 0).slice(4), [
    ["Book", "To do", "7"],
    ["Brief", "Done", "=C5*2"],
    ["Total", "beyond", "the"],
  ]);
  const own = { sheet: 0, box: { top: 6, left: 3, bottom: 6, right: 3 } };
  book.paste(0, { row: 4, column: 3 }, [["=C5*2"]], own);
  assert.equal(texts(book, 0)[3][2], "=C3*2");
  book.paste(0, { row: 1, column: 3 }, [["=C5*2"]], { ...own, cut: true });
  assert.equal(texts(book, 0)[0][2], "=C5*2");
  assert.equal(texts(book, 0)[5][2], "");
});

test("a selection of numbers is counted and summed", () => {
  const book = plan();
  assert.deepEqual(book.summary(0, { top: 4, left: 2, bottom: 5, right: 3 }), {
    count: 2,
    sum: 9600.5,
    average: 4800.25,
  });
  assert.equal(
    book.summary(0, { top: 4, left: 1, bottom: 4, right: 3 }),
    undefined,
  );
  assert.match(
    book.table(1),
    /^<table data-sheet="Inputs">\n<tr><th>Name<th>Value\n/,
  );
});

test("rules strike, fill and embolden what matches, a later rule over an earlier one; one not understood is said", () => {
  const book = openBook(engine, [
    {
      ...tab(
        "Tasks",
        [
          ["#Task", "#Status", "#Cost"],
          ["a", "Done", "-5"],
          ["b", "To do", "2000"],
          ["c", "", "0"],
        ],
        [{}, {}, { "data-rule": "< 0: bad; > 1000: bold; ~ 3: warn" }],
      ),
      attrs: { "data-rule": "B = Done: strike muted" },
    },
  ]);
  assert.deepEqual(book.look(0, 2, 1), {
    fill: "muted",
    bold: false,
    strike: true,
  });
  // The column's own rule gives the fill; the row's strike stays.
  assert.deepEqual(book.look(0, 2, 3), {
    fill: "bad",
    bold: false,
    strike: true,
  });
  assert.deepEqual(book.look(0, 3, 3), {
    fill: undefined,
    bold: true,
    strike: false,
  });
  assert.deepEqual(book.look(0, 4, 3), {
    fill: undefined,
    bold: false,
    strike: false,
  });
  // The header is under no rule.
  assert.equal(book.look(0, 1, 2).strike, false);
  assert.deepEqual(book.errors(), [
    "Tasks: rule `~ 3: warn` is not understood",
  ]);
  // What stops matching loses the look.
  book.paste(0, { row: 2, column: 2 }, [["To do"]]);
  assert.equal(book.look(0, 2, 1).strike, false);
  book.undo();
  // A column put in before B: the rule names its column where it lies now.
  book.insertColumns(0, 2, 1);
  assert.equal(book.tabs[0].attrs["data-rule"], "C = Done: strike muted");
  assert.equal(book.look(0, 2, 1).strike, true);
  assert.equal(book.look(0, 2, 4).fill, "bad");
  assert.match(
    book.table(0),
    /^<table data-sheet="Tasks" data-rule="C = Done: strike muted">\n<col><col><col><col data-rule="< 0: bad; > 1000: bold; ~ 3: warn">\n/,
  );
  // A tab without rules asks nothing of the engine.
  assert.deepEqual(plan().look(0, 4, 3), {});
});

test("a rule that names no column of the tab is reported and applied to nothing; the sheet opens", () => {
  const book = openBook(engine, [
    {
      ...tab("Tasks", [
        ["#Task", "#Status", "#Done"],
        ["a", "Done", "TRUE"],
        ["b", "", "FALSE"],
      ]),
      attrs: {
        "data-rule":
          "Status = Done: strike; not blank: muted; Z blank: bad; C = TRUE: bold; B <> Done: warn",
      },
    },
  ]);
  assert.deepEqual(book.errors(), [
    "Tasks: rule `Status = Done: strike` is not understood",
    "Tasks: rule `not blank: muted` is not understood",
    "Tasks: rule `Z blank: bad` is not understood",
  ]);
  // A ticked box matches TRUE; an empty status is not "other than Done".
  assert.deepEqual(book.look(0, 2, 1), {
    fill: undefined,
    bold: true,
    strike: false,
  });
  assert.deepEqual(book.look(0, 3, 1), {
    fill: undefined,
    bold: false,
    strike: false,
  });
  book.insertColumns(0, 1, 1);
  assert.equal(
    book.tabs[0].attrs["data-rule"],
    "Status = Done: strike; not blank: muted; Z blank: bad; D = TRUE: bold; C <> Done: warn",
  );
});

const charted = (range, attrs = {}) =>
  openBook(engine, [
    {
      ...plan().tabs[0],
      charts: [
        {
          attrs: {
            "data-chart": "bar",
            "data-range": range,
            "data-at": "E4",
            "data-size": "6x12",
            ...attrs,
          },
          caption: "Costs",
        },
      ],
    },
  ]);

test("a chart reads its labels and the columns that hold numbers; a blank cell is a gap, not a zero", () => {
  const book = charted("A3:C6");
  // The Status column is text: it is no series.
  assert.deepEqual(book.chart(0, 0), {
    labels: ["Write", "Book", "Brief"],
    series: [{ name: "Cost", values: [1200, 8400.5, 120] }],
  });
  assert.deepEqual(book.errors(), []);
  book.paste(0, { row: 5, column: 3 }, [[""]]);
  assert.deepEqual(book.chart(0, 0).series[0].values, [1200, undefined, 120]);
});

test("a chart that cannot be drawn says why with the cells in error", () => {
  const said = (...given) => charted(...given).errors();
  assert.deepEqual(said("A3:B6"), ["Tasks: chart A3:B6 holds no numbers"]);
  assert.deepEqual(said("A3:Z9"), [
    "Tasks: chart A3:Z9 names cells outside the tab",
  ]);
  assert.deepEqual(said("the costs"), [
    "Tasks: chart the costs names cells outside the tab",
  ]);
  for (const kind of ["donut", "constructor", ""])
    assert.deepEqual(said("A3:C6", { "data-chart": kind }), [
      "Tasks: chart A3:C6 is of no known kind",
    ]);
  // A pie is its first series: with nothing above zero it has no slice.
  const pie = charted("A3:C6", { "data-chart": "pie" });
  assert.deepEqual(pie.errors(), []);
  pie.paste(0, { row: 4, column: 3 }, [["-1"], ["0"], ["=C4*2"]]);
  assert.deepEqual(pie.errors().slice(0, 1), [
    "Tasks: chart A3:C6 holds no numbers",
  ]);
});

test("a chart follows the rows and columns it draws from, goes with them, and is a step like any other", () => {
  const book = charted("A3:C6");
  const attrs = () => book.tabs[0].charts.map((chart) => chart.attrs);
  book.insertRows(0, 1, 2);
  assert.deepEqual(
    [attrs()[0]["data-range"], attrs()[0]["data-at"]],
    ["A5:C8", "E6"],
  );
  book.insertColumns(0, 2, 1);
  assert.deepEqual(
    [attrs()[0]["data-range"], attrs()[0]["data-at"]],
    ["A5:D8", "F6"],
  );
  assert.match(
    book.table(0),
    /<\/table>\n<figure data-chart="bar" data-range="A5:D8" data-at="F6" data-size="6x12"><figcaption>Costs<\/figcaption><\/figure>$/,
  );
  book.deleteRows(0, 6, 3);
  assert.equal(attrs()[0]["data-range"], "A5:D5");
  book.deleteRows(0, 5, 1);
  assert.deepEqual(attrs(), []);
  book.undo();
  assert.equal(attrs().length, 1);
  book.setCharts(0, (charts) => [
    ...charts,
    { ...charts[0], caption: "Again" },
  ]);
  assert.equal(attrs().length, 2);
  book.undo();
  assert.equal(attrs().length, 1);
  // As sheet.md writes one, with no place said: its range follows all the same.
  const plain = openBook(engine, [
    {
      ...plan().tabs[0],
      charts: [
        { attrs: { "data-chart": "bar", "data-range": "A3:C6" }, caption: "" },
      ],
    },
  ]);
  plain.insertRows(0, 1, 2);
  assert.deepEqual(plain.tabs[0].charts[0].attrs, {
    "data-chart": "bar",
    "data-range": "A5:C8",
  });
});
