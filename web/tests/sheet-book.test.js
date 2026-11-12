// A sheet's tabs and the engine that calculates them (sheet/book.js), with
// the engine itself: what an edit, a moved row or a sort leaves in the file.
import test from "node:test";
import assert from "node:assert/strict";
import { headOf, openBook } from "../sheet/book.js";
import { escaped } from "../sheet/text.js";
import { ask, calculate, engine } from "./sheet-engine.js";

const open = (tabs, locked) => openBook(engine, tabs, locked, calculate);

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
  open([
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

test("the engine says what cells show; what was typed stays as typed", async () => {
  const book = await plan();
  assert.equal(book.shown(0, 1, 2), "2");
  assert.deepEqual(shown(book, 0, 7, 3), ["Total", "", "9,720.50"]);
  assert.equal(book.shown(1, 2, 2), "19,441.00");
  assert.equal(book.shown(1, 3, 2), "4.50%");
  assert.equal(book.tabs[1].rows[2].cells[1].text, "4.5%");
  assert.equal(book.kind(0, 4, 3), "number");
  assert.equal(headOf(book.tabs[0]), 3);
  assert.deepEqual(book.errors(), []);
});

test("a formula shows in its own column's pattern, and without one as the cells it reads do", async () => {
  const book = await open([
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

test("what a typed value brings stands over its column's pattern: a percentage, a date, a currency", async () => {
  const book = await open([
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

test("a typed cell is one step: dependents follow, undo and redo take it back and forth", async () => {
  const book = await plan();
  assert.equal(book.canUndo(), false);
  assert.equal(await book.paste(0, { row: 4, column: 3 }, [["2000"]]), true);
  assert.equal(book.shown(0, 7, 3), "10,600.50");
  assert.equal(book.shown(1, 2, 2), "21,201.00");
  // The same text again is no step; nor is nothing set past the table.
  assert.equal(await book.paste(0, { row: 4, column: 3 }, [["2000"]]), false);
  assert.equal(await book.paste(0, { row: 99, column: 1 }, [[""]]), false);
  assert.equal(await book.undo(), true);
  assert.equal(book.shown(0, 7, 3), "9,720.50");
  assert.equal(book.canUndo(), false);
  assert.equal(await book.redo(), true);
  assert.equal(book.tabs[0].rows[3].cells[2].text, "2000");
  assert.equal(await book.redo(), false);
  assert.equal(await book.paste(0, { row: 6, column: 3 }, [["=1/0"]]), true);
  assert.deepEqual(book.errors(), [
    "Tasks!C6 =1/0 is #DIV/0!",
    "Tasks!C7 =SUM(C4:C6) is #DIV/0!",
    "Inputs!B2 =Tasks!C7*2 is #DIV/0!",
  ]);
});

test("a row put in or taken out moves the formulas under it and on other tabs", async () => {
  const book = await plan();
  await book.insertRows(0, 5, 1);
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
  await book.deleteRows(0, 5, 2);
  assert.deepEqual(texts(book, 0).slice(3), [
    ["Write", "Done", "1200"],
    ["Brief", "To do", "=C4/10"],
    ["Total", "", "=SUM(C4:C5)"],
  ]);
  assert.equal(texts(book, 1)[1][1], "=Tasks!C6*2");
  // Every row cannot go.
  assert.equal(await book.deleteRows(1, 1, 3), false);
  await book.undo();
  await book.undo();
  assert.deepEqual(texts(book, 0), texts(await plan(), 0));
});

test("a column put in keeps a header row a header, and moves formulas and the columns' marks", async () => {
  const book = await plan();
  await book.insertColumns(0, 3, 1);
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
  await book.deleteColumns(0, 1, 1);
  assert.equal(texts(book, 0)[0][0], "=COUNTIF(A4:A6,#REF!)");
  assert.equal(await book.deleteColumns(1, 1, 2), false);
});

test("filling down copies what was typed and moves formulas along", async () => {
  const book = await plan();
  await book.paste(0, { row: 4, column: 3 }, [["=B$3&A4"]]);
  await book.fillDown(0, { top: 4, left: 2, bottom: 6, right: 3 });
  assert.deepEqual(texts(book, 0).slice(3, 6), [
    ["Write", "Done", "=B$3&A4"],
    ["Book", "Done", "=B$3&A5"],
    ["Brief", "Done", "=B$3&A6"],
  ]);
  // One row takes the row above it.
  await book.fillDown(0, { top: 7, left: 1, bottom: 7, right: 1 });
  assert.equal(texts(book, 0)[6][0], "Brief");
});

test("a sort orders the rows under the header, leaves total rows, and keeps each formula on its row", async () => {
  const book = await plan();
  await book.paste(0, { row: 4, column: 3 }, [["=LEN(A4)*1000"], [""]]);
  await book.sort(0, 3, false);
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
  await book.sort(0, 1, true);
  assert.deepEqual(
    texts(book, 0)
      .slice(3, 6)
      .map((row) => row[0]),
    ["Write", "Brief", "Book"],
  );
  assert.equal(texts(book, 0)[3][2], "=LEN(A4)*1000");
  assert.equal(await book.sort(0, 1, true), false);
});

test("a sort that would split a formula's range is refused, with why, and nothing moves", async () => {
  const book = await plan();
  await book.paste(0, { row: 4, column: 3 }, [["=SUM(C4:C5)"]]);
  const before = texts(book, 0);
  assert.equal(
    await book.sort(0, 1, false),
    "Not sorted: =SUM(C4:C5) cannot be moved with its cell.",
  );
  assert.deepEqual(texts(book, 0), before);
  // The next change is made as any other.
  assert.equal(
    await book.fillDown(0, { top: 5, left: 1, bottom: 6, right: 1 }),
    true,
  );
});

test("a row put in leaves a formula it does not move exactly as typed", async () => {
  const typed = ["=1e3", "=MYFUNC(A1)", "=A1 B1", "=SUM(A5:A6)"];
  const book = await open([
    tab("Odd", [...typed.map((text) => [text]), ["1"], ["2"]]),
  ]);
  await book.insertRows(0, 6, 1);
  assert.deepEqual(
    texts(book, 0).map(([text]) => text),
    [...typed.slice(0, 3), "=SUM(A5:A7)", "1", "", "2"],
  );
});

test("tabs may bear the names the engine gives its own sheets", async () => {
  const book = await open([
    tab("Sheet2", [["=Sheet1!A1+1"]]),
    tab("Sheet1", [["41"]]),
  ]);
  assert.equal(book.shown(0, 1, 1), "42");
});

test("numbers of twelve digits sort and sum exactly", async () => {
  const book = await open([
    tab("Big", [["#N"], ["100000000002"], ["100000000001"], ["100000000003"]]),
  ]);
  assert.equal(
    book.summary(0, { top: 2, left: 1, bottom: 4, right: 1 }).sum,
    300000000006,
  );
  await book.sort(0, 1, false);
  assert.deepEqual(texts(book, 0).flat(), [
    "N",
    "100000000001",
    "100000000002",
    "100000000003",
  ]);
});

test("a tab with markup in a cell says so with the cells in error, and no change to it is made", async () => {
  const marked = {
    ...tab("Costs", [
      ["1", "=IF(A1"],
      ["2", "x"],
    ]),
    markup: ["B1"],
  };
  const book = await open([marked, tab("Other", [["=Costs!A2"]])]);
  const why = "Costs!B1 holds markup: write < as &lt;";
  assert.equal(book.errors()[0], why);
  assert.equal(await book.paste(0, { row: 2, column: 2 }, [["y"]]), why);
  assert.equal(await book.insertRows(0, 1, 1), why);
  // The engine is as it was: the other tab still reads the same cell.
  assert.equal(book.shown(1, 1, 1), "2");
  assert.equal(book.canUndo(), false);
  assert.equal(await book.paste(1, { row: 1, column: 1 }, [["7"]]), true);
});

test("undo keeps the last hundred steps and lets go of the oldest", async () => {
  const book = await plan();
  for (let step = 0; step <= 100; step++)
    assert.equal(
      await book.paste(0, { row: 2, column: 1 }, [[`${step}`]]),
      true,
    );
  let undone = 0;
  while (await book.undo()) undone++;
  assert.equal(undone, 100);
  assert.equal(texts(book, 0)[1][0], "0");
});

test("text set past the table makes it reach there, a header row staying one; blank rows and columns at its end are never kept", async () => {
  const book = await open([
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
  await book.paste(0, { row: 3, column: 2 }, [["2000"]]);
  assert.equal(book.look(0, 3, 2).bold, true);
  await book.undo();
  const before = book.table(0);
  assert.equal(await book.paste(0, { row: 4, column: 4 }, [["=B2*2"]]), true);
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
  await book.undo();
  assert.equal(book.table(0), before);
  await book.redo();
  assert.equal(await book.paste(0, { row: 4, column: 4 }, [[""]]), true);
  assert.equal(book.table(0), before);
  // Filled down past its end, it reaches as far as what is filled.
  await book.fillDown(0, { top: 2, left: 1, bottom: 4, right: 3 });
  assert.deepEqual(texts(book, 0).slice(2), [
    ["Rent", "1200"],
    ["Rent", "1200"],
  ]);
  // Too far from the table, what is typed is refused: the file would hold
  // every blank row between, and the host keeps no file that large.
  const refusal = /^B100000 is too far from the table to keep/;
  assert.match(
    await book.paste(0, { row: 100_000, column: 2 }, [["x"]]),
    refusal,
  );
  assert.match(
    await book.fillDown(0, { top: 2, left: 2, bottom: 100_000, right: 2 }),
    /^B100000 is too far/,
  );
  assert.equal(await book.paste(0, { row: 30_000, column: 2 }, [["x"]]), true);
  await book.undo();
  // The host counts bytes: text of three bytes a character is a third as
  // many characters.
  const large = (text) => book.paste(0, { row: 2, column: 1 }, [[text]]);
  assert.equal(await large("a".repeat(900_000)), true);
  assert.equal(await large("語".repeat(300_000)), true);
  assert.match(
    await book.paste(0, { row: 9_000, column: 2 }, [["x"]]),
    /^B9000 is too far/,
  );
  await book.undo();
  await book.undo();
  // A blank row put in at its end is no change, and no step.
  const steps = async () => {
    let count = 0;
    while (await book.undo()) count++;
    return count;
  };
  assert.equal(await book.insertRows(0, 5, 2), false);
  assert.equal(await steps(), 3);
});

test("pasted text lands from a cell on; this book's own cells bring their formulas, and a cut clears", async () => {
  const book = await plan();
  await book.paste(0, { row: 5, column: 2 }, [
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
  await book.paste(0, { row: 4, column: 3 }, [["=C5*2"]], own);
  assert.equal(texts(book, 0)[3][2], "=C3*2");
  await book.paste(0, { row: 1, column: 3 }, [["=C5*2"]], {
    ...own,
    cut: true,
  });
  assert.equal(texts(book, 0)[0][2], "=C5*2");
  assert.equal(texts(book, 0)[5][2], "");
});

test("a selection of numbers is counted and summed", async () => {
  const book = await plan();
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

test("rules strike, fill and embolden what matches, a later rule over an earlier one; one not understood is said", async () => {
  const book = await open([
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
  assert.deepEqual(book.look(0, 1, 2), {});
  assert.deepEqual(book.errors(), [
    "Tasks: rule `~ 3: warn` is not understood",
  ]);
  // What stops matching loses the look.
  await book.paste(0, { row: 2, column: 2 }, [["To do"]]);
  assert.equal(book.look(0, 2, 1).strike, false);
  await book.undo();
  // A column put in before B: the rule names its column where it lies now.
  await book.insertColumns(0, 2, 1);
  assert.equal(book.tabs[0].attrs["data-rule"], "C = Done: strike muted");
  assert.equal(book.look(0, 2, 1).strike, true);
  assert.equal(book.look(0, 2, 4).fill, "bad");
  assert.match(
    book.table(0),
    /^<table data-sheet="Tasks" data-rule="C = Done: strike muted">\n<col><col><col><col data-rule="< 0: bad; > 1000: bold; ~ 3: warn">\n/,
  );
  // A tab without rules has no look anywhere.
  assert.deepEqual((await plan()).look(0, 4, 3), {});
});

test("a rule that names no column of the grid is reported and applied to nothing; the sheet opens", async () => {
  const book = await open([
    {
      ...tab("Tasks", [
        ["#Task", "#Status", "#Done"],
        ["a", "Done", "TRUE"],
        ["b", "", "FALSE"],
      ]),
      attrs: {
        "data-rule":
          "Status = Done: strike; not blank: muted; ZZ blank: bad; C = TRUE: bold; B <> Done: warn",
      },
    },
  ]);
  assert.deepEqual(book.errors(), [
    "Tasks: rule `Status = Done: strike` is not understood",
    "Tasks: rule `not blank: muted` is not understood",
    "Tasks: rule `ZZ blank: bad` is not understood",
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
  await book.insertColumns(0, 1, 1);
  assert.equal(
    book.tabs[0].attrs["data-rule"],
    "Status = Done: strike; not blank: muted; ZZ blank: bad; D = TRUE: bold; C <> Done: warn",
  );
});

const charted = async (range, attrs = {}) =>
  open([
    {
      ...(await plan()).tabs[0],
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

test("tabs are put in, copied, moved, named and taken out, each a step; a new name is written into the formulas that read the tab", async () => {
  const book = await plan();
  const names = () => book.tabs.map((tab) => tab.name);
  assert.equal(book.readers(0), 1);
  assert.equal(book.readers(1), 0);
  assert.equal(await book.nameTab(0, "Jobs & more"), true);
  assert.equal(book.tabs[1].rows[1].cells[1].text, "='Jobs & more'!C7*2");
  assert.equal(book.shown(1, 2, 2), "19,441.00");
  assert.equal(book.readers(0), 1);
  // A name another tab has, or one a spreadsheet does not take, is refused.
  for (const name of ["inputs", "", "a/b", "x".repeat(32)])
    assert.match(await book.nameTab(0, name), /^A tab's name/);
  assert.equal(await book.nameTab(0, "Jobs & more"), false);
  await book.undo();
  assert.equal(book.tabs[1].rows[1].cells[1].text, "=Tasks!C7*2");
  assert.equal(await book.addTab(), true);
  assert.equal(await book.copyTab(1), true);
  assert.deepEqual(names(), ["Tasks", "Inputs", "Inputs copy", "Sheet"]);
  assert.equal(book.shown(2, 2, 2), "19,441.00");
  assert.equal(await book.moveTab(3, -1), true);
  assert.equal(await book.moveTab(0, -1), false);
  assert.deepEqual(names(), ["Tasks", "Inputs", "Sheet", "Inputs copy"]);
  // Taken out, the cells that read it are in error; the last tab stays.
  assert.equal(await book.dropTab(0), true);
  assert.equal(book.kind(0, 2, 2), "error");
  for (const _ of [1, 2]) await book.dropTab(0);
  assert.match(await book.dropTab(0), /at least one tab/);
  for (let steps = 0; steps < 6; steps++) await book.undo();
  assert.deepEqual(names(), ["Tasks", "Inputs"]);
  assert.equal(book.shown(1, 2, 2), "19,441.00");
});

test("a look is set on a box's cells and taken off where all have it; the table reaches for it, and a far cell is refused", async () => {
  const book = await plan();
  const none = new Set();
  const box = { top: 4, left: 1, bottom: 5, right: 2 };
  const attrs = (row, column) =>
    book.tabs[0].rows[row - 1].cells[column - 1].attrs;
  assert.equal(await book.style(0, box, "bold", "bold", new Set([5])), true);
  assert.deepEqual([attrs(4, 2), attrs(5, 2)], [{ class: "bold" }, {}]);
  // Not all have it: the rest take it. All have it: it goes.
  assert.equal(await book.style(0, box, "bold", "bold", none), true);
  assert.equal(await book.style(0, box, "align", "center", none), true);
  assert.equal(await book.style(0, box, "align", "right", none), true);
  assert.deepEqual(attrs(5, 1), { class: "bold right" });
  assert.equal(await book.style(0, box, "bold", "bold", none), true);
  assert.deepEqual(attrs(5, 1), { class: "right" });
  assert.equal(await book.style(0, box, "align", "right", none), true);
  assert.deepEqual(attrs(5, 1), {});
  // A header is bold whatever its class says, and a number stands right.
  assert.equal(book.state(0, 3, 1).bold, true);
  assert.equal(book.state(0, 4, 3).align, "right");
  assert.equal(book.state(0, 4, 1).align, "left");
  assert.equal(book.state(0, 40, 9).format, "general");

  assert.equal(await book.style(0, box, "fill", "good", none), true);
  assert.equal(await book.style(0, box, "ink", "bad", none), true);
  assert.deepEqual(attrs(4, 1), { "data-fill": "good", "data-ink": "bad" });
  assert.equal(book.marks(0, 4, 1).fill, "good");
  assert.equal(await book.style(0, box, "fill", "", none), true);
  assert.deepEqual(attrs(4, 1), { "data-ink": "bad" });
  assert.equal(await book.style(0, box, "fill", "", none), false);
  // Only the theme's names and the grid's words are taken.
  assert.equal(await book.style(0, box, "fill", "#f00", none), false);
  assert.equal(await book.style(0, box, "align", "bold", none), false);

  // A cell's own pattern stands over what was typed in it; General over
  // its column's.
  const rate = { top: 3, left: 2, bottom: 3, right: 2 };
  assert.equal(await book.style(1, rate, "format", "0.000", none), true);
  assert.equal(book.shown(1, 3, 2), "0.045");
  assert.equal(book.state(1, 3, 2).format, "0.000");
  const cost = { top: 5, left: 3, bottom: 5, right: 3 };
  assert.equal(await book.style(0, cost, "format", "general", none), true);
  assert.equal(book.shown(0, 5, 3), "8400.5");

  // Past the table it reaches there, as text typed there does.
  const past = { top: 9, left: 4, bottom: 9, right: 4 };
  assert.equal(await book.style(0, past, "wrap", "wrap", none), true);
  assert.equal(book.tabs[0].rows.length, 9);
  assert.deepEqual(attrs(9, 4), { class: "wrap" });
  assert.equal(book.marks(0, 9, 4).wrap, true);
  assert.equal(await book.style(0, past, "wrap", "wrap", none), true);
  assert.equal(book.tabs[0].rows.length, 7);
  const far = { top: 1, left: 1, bottom: 1_048_576, right: 16_384 };
  assert.match(await book.style(0, far, "bold", "bold", none), /too far/);
  assert.equal(await book.style(0, far, "ink", "", none), true);
  assert.deepEqual(attrs(4, 1), {});
  await book.undo();
  assert.deepEqual(attrs(4, 1), { "data-ink": "bad" });
});

test("the table a cell being typed in would make, cut around its text, joins to the one its edit makes; no step is made", async () => {
  const book = await plan();
  for (const [at, text] of [
    [{ row: 4, column: 1 }, "Draft"],
    [{ row: 5, column: 3 }, "=C4*2"],
    [{ row: 4, column: 2 }, "a < b & c"],
    [{ row: 9, column: 5 }, "far"],
    [{ row: 9, column: 5 }, ""],
    [{ row: 7, column: 3 }, ""],
    [{ row: 6, column: 2 }, ""],
  ]) {
    const before = book.table(0);
    const { head, tail, empty = head + tail } = book.draft(0, at);
    assert.equal(book.table(0), before);
    assert.equal(book.canUndo(), false);
    const taken = await book.paste(0, at, [[text]]);
    assert.equal(text ? head + escaped(text) + tail : empty, book.table(0));
    if (taken === true) await book.undo();
  }
  assert.equal(book.draft(0, { row: 900_000, column: 1 }), undefined);
});

test("rows and columns move among their neighbours: a reference follows its cell on every tab, a range where its ends moved alike, a chart its cells, a rule its column; a header row and a total row stay", async () => {
  const costs = tab(
    "Costs",
    [
      ["#Item", "#Net", "#Tax", "#Sum"],
      ["Rent", "1200", "240", "=B2+C2"],
      ["Power", "90", "18", "=B3+C3"],
      ["Water", "60", "12", "=SUM(B2:B4)"],
    ],
    [{}, { "data-format": "0.00" }, {}, {}],
  );
  const chart = { "data-chart": "bar", "data-range": "B1:C4", "data-at": "F3" };
  const book = await open([
    { ...costs, charts: [{ attrs: chart, caption: "" }] },
    tab("Notes", [["=Costs!B2", "=SUM(Costs!B2:B3)", "=costs!$C$2"]]),
  ]);
  const charted = () => book.tabs[0].charts[0].attrs;
  await book.mark(0, "data-rule", "C > 100: bold");
  // The second row goes down one: it and what read it are a row on.
  assert.equal(await book.moveRows(0, 2, 1, 1), true);
  assert.deepEqual(texts(book, 0), [
    ["Item", "Net", "Tax", "Sum"],
    ["Power", "90", "18", "=B2+C2"],
    ["Rent", "1200", "240", "=B3+C3"],
    ["Water", "60", "12", "=SUM(B2:B4)"],
  ]);
  assert.equal(book.shown(0, 3, 4), "1440.00");
  // The chart's range runs between where its ends went, here where they
  // were, and it lies on the cell it lay on, a row down.
  assert.deepEqual(charted(), { ...chart, "data-at": "F2" });
  // Up again, and no further than under the header.
  assert.equal(await book.moveRows(0, 3, 1, -1), true);
  assert.match(await book.moveRows(0, 2, 1, -1), /header row keeps its place/);
  assert.equal(await book.moveRows(0, 4, 1, 1), false);
  // Two columns go left one: the pattern goes with its column, the rule
  // names the column where it now is.
  assert.equal(await book.moveColumns(0, 3, 2, -1), true);
  assert.deepEqual(texts(book, 0)[1], ["Rent", "240", "=D2+B2", "1200"]);
  assert.deepEqual(book.tabs[0].columns[3], { "data-format": "0.00" });
  assert.equal(book.tabs[0].attrs["data-rule"], "B > 100: bold");
  assert.equal(book.look(0, 2, 2).bold, true);
  assert.equal(await book.moveColumns(0, 1, 1, -1), false);
  assert.deepEqual(texts(book, 0)[3], ["Water", "12", "=SUM(D2:D4)", "60"]);
  // Its two columns moved apart: the range takes in what lies between,
  // so both are still drawn. Moved alike, it goes along as it is.
  assert.equal(charted()["data-range"], "B1:D4");
  await book.undo();
  assert.equal(await book.moveColumns(0, 2, 2, 1), true);
  assert.equal(charted()["data-range"], "C1:D4");
  await book.undo();
  assert.equal(await book.moveColumns(0, 3, 2, -1), true);
  // Another tab's formulas follow the cells they read, a range with them.
  assert.deepEqual(texts(book, 1), [
    ["=Costs!D2", "=SUM(Costs!D2:D3)", "=costs!$B$2"],
  ]);
  await book.undo();
  assert.deepEqual(texts(book, 0)[1], ["Rent", "1200", "240", "=B2+C2"]);
  assert.equal(await book.moveRows(0, 2, 2, 1), true);
  assert.deepEqual(texts(book, 1), [
    ["=Costs!B3", "=SUM(Costs!B3:B4)", "=costs!$C$3"],
  ]);
  // No row is moved onto a total row, nor the total row itself.
  const tasks = await plan();
  assert.match(await tasks.moveRows(0, 6, 1, 1), /total row keeps its place/);
  assert.match(await tasks.moveRows(0, 7, 1, -1), /total row keeps its place/);
  assert.equal(await tasks.moveRows(0, 5, 1, 1), true);
});

test("a column is as wide as said or as its texts; looks are taken off a box's cells, and off a whole column its own", async () => {
  const book = await plan();
  const none = new Set();
  assert.equal(await book.widen(0, { left: 1, right: 2 }, 180), true);
  assert.deepEqual(
    book.tabs[0].columns.map((column) => column["data-width"]),
    ["180", "180", undefined],
  );
  assert.equal(await book.widen(0, { left: 2, right: 3 }), true);
  assert.deepEqual(book.tabs[0].columns[1], {});
  assert.equal(await book.widen(0, { left: 2, right: 3 }), false);

  const box = { top: 4, left: 3, bottom: 5, right: 3 };
  for (const [name, value] of [
    ["bold", "bold"],
    ["align", "center"],
    ["fill", "good"],
    ["format", "0%"],
  ])
    await book.style(0, box, name, value, none);
  assert.equal(await book.unstyle(0, box, new Set([5])), true);
  const attrs = (row) => book.tabs[0].rows[row - 1].cells[2].attrs;
  assert.deepEqual(attrs(4), {});
  assert.equal(attrs(5)["data-fill"], "good");
  // A box of part of the column leaves the column's pattern; all of it
  // takes that too.
  assert.equal(await book.unstyle(0, { ...box, top: 5 }, none), true);
  assert.equal(await book.unstyle(0, { ...box, top: 5 }, none), false);
  assert.equal(book.tabs[0].columns[2]["data-format"], "#,##0.00");
  assert.equal(
    await book.unstyle(0, { ...box, top: 1, bottom: 7 }, none),
    true,
  );
  assert.deepEqual(book.tabs[0].columns[2], {});
  assert.equal(book.shown(0, 4, 3), "1200");
  // A row's own fill goes with all of the row, and stays with part of it.
  const filled = tab("Costs", [
    ["#Item", "#Cost"],
    ["Rent", "1200"],
  ]);
  filled.rows[1].attrs = { "data-fill": "warn" };
  const costs = await open([filled]);
  const fill = () => costs.tabs[0].rows[1].attrs["data-fill"];
  const row = { top: 2, left: 1, bottom: 2, right: 2 };
  assert.equal(await costs.unstyle(0, { ...row, left: 2 }, none), false);
  assert.equal(fill(), "warn");
  assert.equal(await costs.unstyle(0, row, none), true);
  assert.equal(fill(), undefined);
});

test("a workbook sent out carries the looks written: bold, italic, the colours, where the text stands and wrap", async () => {
  const book = await plan();
  const at = { top: 4, left: 1, bottom: 4, right: 1 };
  for (const [name, value] of [
    ["bold", "bold"],
    ["italic", "italic"],
    ["wrap", "wrap"],
    ["align", "center"],
    ["fill", "good"],
    ["ink", "bad"],
  ])
    assert.equal(await book.style(0, at, name, value, new Set()), true);
  const read = engine.Model.fromXlsx(
    await ask({ tabs: book.tabs, xlsx: true }),
    "Plan",
    "en",
    "UTC",
    "en",
  );
  const styled = (row) => read.getCellStyle(0, row, 1).style;
  const { font, fill, alignment } = styled(4);
  assert.deepEqual(
    [font.b, font.i, font.color, fill.color, alignment],
    [
      true,
      true,
      "#B31D28",
      "#DCF5E3",
      { horizontal: "center", wrap_text: true },
    ],
  );
  // A header is bold there as here; a plain cell is plain.
  assert.equal(styled(3).font.b, true);
  assert.equal(!!styled(5).font.b, false);
  read.free();
});

test("a chart reads its labels and the columns that hold numbers; a blank cell is a gap, not a zero", async () => {
  const book = await charted("A3:C6");
  // The Status column is text: it is no series.
  assert.deepEqual(book.chart(0, 0), {
    labels: ["Write", "Book", "Brief"],
    series: [{ name: "Cost", values: [1200, 8400.5, 120] }],
  });
  assert.deepEqual(book.errors(), []);
  await book.paste(0, { row: 5, column: 3 }, [[""]]);
  assert.deepEqual(book.chart(0, 0).series[0].values, [1200, undefined, 120]);
});

test("a chart whose range reaches past the table reads blank cells there, as when blank rows under it are cut", async () => {
  const book = await open([
    {
      ...tab("Costs", [
        ["#Item", "#Net"],
        ["Rent", "1200"],
        ["Power", "90"],
        ["", ""],
        ["", ""],
      ]),
      charts: [
        {
          attrs: { "data-chart": "bar", "data-range": "A1:B5" },
          caption: "",
        },
      ],
    },
  ]);
  await book.paste(0, { row: 2, column: 2 }, [["1300"]]);
  assert.equal(book.tabs[0].rows.length, 3);
  assert.deepEqual(book.chart(0, 0).series[0].values, [
    1300,
    90,
    undefined,
    undefined,
  ]);
  assert.deepEqual(book.errors(), []);
});

test("a chart that cannot be drawn says why with the cells in error", async () => {
  const said = async (...given) => (await charted(...given)).errors();
  assert.deepEqual(await said("A3:B6"), [
    "Tasks: chart A3:B6 holds no numbers",
  ]);
  assert.deepEqual(await said("the costs"), [
    "Tasks: chart the costs names no cells",
  ]);
  // Past the table a range reads blank cells: the chart is of the rest.
  assert.deepEqual(await said("A3:Z9"), []);
  for (const kind of ["donut", "constructor", ""])
    assert.deepEqual(await said("A3:C6", { "data-chart": kind }), [
      "Tasks: chart A3:C6 is of no known kind",
    ]);
  // A pie is its first series: with nothing above zero it has no slice.
  const pie = await charted("A3:C6", { "data-chart": "pie" });
  assert.deepEqual(pie.errors(), []);
  await pie.paste(0, { row: 4, column: 3 }, [["-1"], ["0"], ["=C4*2"]]);
  assert.deepEqual(pie.errors().slice(0, 1), [
    "Tasks: chart A3:C6 holds no numbers",
  ]);
});

test("a chart follows the rows and columns it draws from, goes with them, and is a step like any other", async () => {
  const book = await charted("A3:C6");
  const attrs = () => book.tabs[0].charts.map((chart) => chart.attrs);
  await book.insertRows(0, 1, 2);
  assert.deepEqual(
    [attrs()[0]["data-range"], attrs()[0]["data-at"]],
    ["A5:C8", "E6"],
  );
  await book.insertColumns(0, 2, 1);
  assert.deepEqual(
    [attrs()[0]["data-range"], attrs()[0]["data-at"]],
    ["A5:D8", "F6"],
  );
  assert.match(
    book.table(0),
    /<\/table>\n<figure data-chart="bar" data-range="A5:D8" data-at="F6" data-size="6x12"><figcaption>Costs<\/figcaption><\/figure>$/,
  );
  await book.deleteRows(0, 6, 3);
  assert.equal(attrs()[0]["data-range"], "A5:D5");
  await book.deleteRows(0, 5, 1);
  assert.deepEqual(attrs(), []);
  await book.undo();
  assert.equal(attrs().length, 1);
  await book.setCharts(0, (charts) => [
    ...charts,
    { ...charts[0], caption: "Again" },
  ]);
  assert.equal(attrs().length, 2);
  await book.undo();
  assert.equal(attrs().length, 1);
  // As sheet.md writes one, with no place said: its range follows all the same.
  const plain = await open([
    {
      ...(await plan()).tabs[0],
      charts: [
        { attrs: { "data-chart": "bar", "data-range": "A3:C6" }, caption: "" },
      ],
    },
  ]);
  await plain.insertRows(0, 1, 2);
  assert.deepEqual(plain.tabs[0].charts[0].attrs, {
    "data-chart": "bar",
    "data-range": "A5:C8",
  });
});

test("a column's rule is set as one step: its cells take the look, a far column is reached for its own, and one not understood is refused", async () => {
  const book = await plan();
  const tasks = () => book.tabs[0];
  // On the costs: one cell is over a thousand, a formula's result too.
  const added = (column, rule) =>
    book.setRule(0, column, book.rules(0, column).length, undefined, rule);
  assert.equal(await added(3, { text: "> 1000: bad", row: false }), true);
  assert.equal(tasks().columns[2]["data-rule"], "> 1000: bad");
  assert.deepEqual(
    [4, 5, 6].map((row) => book.look(0, row, 3).fill),
    ["bad", "bad", undefined],
  );
  assert.deepEqual(book.rules(0, 3), [
    { text: "> 1000: bad", row: false, condition: "> 1000", effects: ["bad"] },
  ]);
  // On the rows by their status, then taken out again.
  await added(2, { text: "= Done: strike", row: true });
  assert.equal(tasks().attrs["data-rule"], "B = Done: strike");
  assert.equal(book.look(0, 4, 1).strike, true);
  // Its place is taken with what stands there: another text is refused.
  assert.match(await book.setRule(0, 2, 0, "= Open: strike"), /no longer/);
  assert.equal(await book.setRule(0, 2, 1), false);
  await book.setRule(0, 2, 0, "= Done: strike");
  assert.equal(tasks().attrs["data-rule"], undefined);
  for (const _ of "123") await book.undo();
  assert.equal(book.canUndo(), false);
  assert.equal(tasks().columns[2]["data-rule"], undefined);
  // A column past the table is reached for a rule of its own.
  await added(6, { text: "blank: muted", row: false });
  assert.deepEqual(tasks().columns.slice(3), [
    {},
    {},
    { "data-rule": "blank: muted" },
  ]);
  // Not understood, or of a column too far to test: nothing is written.
  const before = book.table(0);
  for (const [column, rule, why] of [
    [3, { text: "> many: bad", row: false }, /does not understand/],
    [3, { text: "> 1: sparkle", row: true }, /does not understand/],
    [60, { text: "> 1: bad", row: true }, /column BH is too far/],
  ])
    assert.match(await added(column, rule), why);
  assert.equal(book.table(0), before);
  // Of its own, the far column is reached.
  assert.equal(await added(60, { text: "> 1: bad", row: false }), true);
  assert.equal(book.tabs[0].columns[59]["data-rule"], "> 1: bad");
});

// The engine panics on XNPV of two plain numbers.
const PANICS = /^Not changed: the engine cannot calculate/;

test("a change the engine panics on is refused: the tabs, the steps and what they show stand", async () => {
  const book = await open([
    tab("Costs", [
      ["#Rate", "#Flow", "#Worth"],
      ["1", "", "=XNPV(A2,B2)"],
      ["2", "3", "=A3+B3"],
    ]),
  ]);
  const before = book.table(0);
  const refused = (changed) => {
    assert.match(changed, PANICS);
    assert.equal(book.table(0), before);
    assert.equal(book.canUndo(), false);
    assert.deepEqual(shown(book, 0, 3, 3), ["2", "3", "5"]);
  };
  // A formula typed, one moved along, and a value another formula reads.
  refused(await book.paste(0, { row: 2, column: 4 }, [["=XNPV(1,1)"]]));
  refused(await book.fillDown(0, { top: 2, left: 3, bottom: 3, right: 3 }));
  refused(await book.paste(0, { row: 2, column: 2 }, [["1"]]));
  // A row put in by the engine itself moves the formula to where it panics.
  const moved = await open([
    tab("Costs", [['=XNPV(1,IF(ROW()=1,"x",1))'], ["2"]]),
  ]);
  assert.equal(moved.shown(0, 1, 1), "#NUM!");
  assert.match(await moved.insertRows(0, 1, 1), PANICS);
  assert.deepEqual(shown(moved, 0, 2, 1), ["2"]);
  assert.equal(await moved.insertRows(0, 2, 1), true);
  assert.equal(moved.tabs[0].rows.length, 3);
  // What it takes is a step as ever, and Undo and Redo go over it.
  assert.equal(await book.paste(0, { row: 2, column: 2 }, [["x"]]), true);
  assert.equal(book.shown(0, 2, 3), "#NUM!");
  assert.equal(await book.undo(), true);
  assert.equal(await book.redo(), true);
});

test("a file with a formula the engine panics on is not opened", async () => {
  const rows = [
    ["#Rate", "#Worth"],
    ["1", "=XNPV(A2,1)"],
    ["2", "=A3*2"],
  ];
  assert.match(await open([tab("Costs", rows)]), PANICS);
});

// The engine never returns from the factorial of so large a number: its
// worker is let go once its time is up.
const LATE = "Not changed: the engine has not finished calculating.";
const timed = (tabs) =>
  openBook(
    engine,
    tabs,
    false,
    async (next) => (await ask({ tabs: next }, 1000)) ?? LATE,
  );

test("a change the engine does not finish calculating is refused, and a file that holds one is not opened", async () => {
  const book = await timed([
    tab("Costs", [
      ["#N", "#Ways"],
      ["5", "=FACT(A2)"],
    ]),
  ]);
  const before = book.table(0);
  // A formula typed, and a value one reads.
  assert.equal(
    await book.paste(0, { row: 2, column: 3 }, [["=FACT(99999999999)"]]),
    LATE,
  );
  assert.equal(
    await book.paste(0, { row: 2, column: 1 }, [["99999999999"]]),
    LATE,
  );
  assert.equal(book.table(0), before);
  assert.equal(book.canUndo(), false);
  assert.equal(book.shown(0, 2, 2), "120");
  assert.equal(await timed([tab("Costs", [["=FACT(99999999999)"]])]), LATE);
});

// Rows to point at, by what they hold.
const rows = () =>
  open([tab("Rows", [["#N"], ["r2"], ["r3"], ["r4"], ["r5"]])]);
const column = (book) => texts(book, 0).map(([text]) => text);

test("a change shows as soon as it is asked for, and what is asked next is made on what shows: what is pointed at is what is changed", async () => {
  const deleted = await rows();
  const asked = [
    deleted.deleteRows(0, 2, 1),
    deleted.paste(0, { row: 4, column: 1 }, [["X"]]),
  ];
  assert.deepEqual(column(deleted), ["N", "r3", "r4", "X"]);
  assert.deepEqual(await Promise.all(asked), [true, true]);
  assert.deepEqual(column(deleted), ["N", "r3", "r4", "X"]);
  assert.equal(deleted.made, deleted.tabs);
  // A row put in above, then typed in.
  const inserted = await rows();
  await Promise.all([
    inserted.insertRows(0, 2, 1),
    inserted.paste(0, { row: 2, column: 1 }, [["new"]]),
  ]);
  assert.deepEqual(column(inserted), ["N", "new", "r2", "r3", "r4", "r5"]);
  // Sorted, then typed in; moved, then pasted on.
  const sorted = await rows();
  await Promise.all([
    sorted.sort(0, 1, true),
    sorted.paste(0, { row: 2, column: 1 }, [["top"]]),
  ]);
  assert.deepEqual(column(sorted), ["N", "top", "r4", "r3", "r2"]);
  const moved = await rows();
  await Promise.all([
    moved.moveRows(0, 2, 1, 1),
    moved.paste(0, { row: 3, column: 1 }, [["moved"]]),
  ]);
  assert.deepEqual(column(moved), ["N", "r3", "moved", "r4", "r5"]);
  // Undo behind a change takes it back as it is asked for.
  const undone = await rows();
  const back = [undone.paste(0, { row: 2, column: 1 }, [["x"]]), undone.undo()];
  assert.deepEqual(column(undone), ["N", "r2", "r3", "r4", "r5"]);
  assert.deepEqual(await Promise.all(back), [true, true]);
  assert.equal(undone.canRedo(), true);
});

test("a cell not yet calculated shows as typed, a formula blank; one a move only rewrote keeps its value", async () => {
  const book = await open([
    tab("Costs", [
      ["#N", "#Twice"],
      ["2", "=A2*2"],
    ]),
  ]);
  const asked = [
    book.insertRows(0, 2, 1),
    book.paste(0, { row: 2, column: 1 }, [["7"]]),
    book.paste(0, { row: 2, column: 2 }, [["=A2*3"]]),
  ];
  assert.deepEqual(shown(book, 0, 2, 2), ["7", ""]);
  assert.deepEqual(shown(book, 0, 3, 2), ["2", "4"]);
  assert.equal(texts(book, 0)[2][1], "=A3*2");
  await Promise.all(asked);
  assert.deepEqual(shown(book, 0, 2, 2), ["7", "21"]);
});

test("a formula a sort moved keeps its value until it is calculated again", async () => {
  const book = await open([
    tab("Costs", [
      ["#N", "#Twice"],
      ["3", "=A2*2"],
      ["1", "=A3*2"],
    ]),
  ]);
  const sorted = book.sort(0, 1, false);
  assert.deepEqual(texts(book, 0).slice(1), [
    ["1", "=A2*2"],
    ["3", "=A3*2"],
  ]);
  assert.deepEqual(shown(book, 0, 2, 2), ["1", "2"]);
  assert.equal(await sorted, true);
  assert.deepEqual(shown(book, 0, 2, 2), ["1", "2"]);
});

test("a change refused is taken back alone: those asked for behind it that move nothing are made again on the tabs calculated", async () => {
  const book = await timed([
    tab("Rows", [
      ["#N", "#M"],
      ["r2", ""],
      ["r3", ""],
      ["r4", ""],
    ]),
  ]);
  const box = { top: 3, left: 1, bottom: 3, right: 1 };
  const asked = [
    book.paste(0, { row: 2, column: 2 }, [["=FACT(99999999999)"]]),
    book.paste(0, { row: 3, column: 2 }, [["typed"]]),
    book.paste(0, { row: 4, column: 2 }, [["=A4"]]),
    book.style(0, box, "bold", "bold", new Set()),
  ];
  assert.deepEqual(await Promise.all(asked), [LATE, true, true, true]);
  assert.deepEqual(texts(book, 0).slice(1), [
    ["r2", ""],
    ["r3", "typed"],
    ["r4", "=A4"],
  ]);
  assert.equal(book.shown(0, 4, 2), "r4");
  assert.equal(book.marks(0, 3, 1).bold, true);
  assert.equal(book.made, book.tabs);
  // A paste refused moved nothing: a row put in behind it is made again,
  // and what was typed in that row.
  const moved = await rows();
  const behind = [
    moved.paste(0, { row: 2, column: 1 }, [["=XNPV(1,1)"]]),
    moved.insertRows(0, 2, 1),
    moved.paste(0, { row: 2, column: 1 }, [["above"]]),
  ];
  const [first, ...made] = await Promise.all(behind);
  assert.match(first, PANICS);
  assert.deepEqual(made, [true, true]);
  assert.deepEqual(column(moved), ["N", "above", "r2", "r3", "r4", "r5"]);
});

test("a change refused that moved cells takes back those behind it, said once with how many", async () => {
  // Put in above it, the formula no longer stands in row 2.
  const formula = "=IF(ROW()=2,1,FACT(99999999999))";
  const book = await timed([tab("Rows", [["#N"], [formula], ["r3"]])]);
  const asked = [
    book.insertRows(0, 2, 1),
    book.paste(0, { row: 2, column: 1 }, [["typed"]]),
  ];
  assert.deepEqual(await Promise.all(asked), [
    `${LATE} A change asked for after it was not made either.`,
    false,
  ]);
  assert.deepEqual(column(book), ["N", formula, "r3"]);
});

test("a sort asked for while a change is calculated is refused: the values it orders by are not known yet", async () => {
  const book = await rows();
  const asked = [
    book.paste(0, { row: 2, column: 1 }, [["z"]]),
    book.sort(0, 1, false),
  ];
  assert.deepEqual(await Promise.all(asked), [
    true,
    "Not changed: the values it reads are still being calculated. Ask again in a moment.",
  ]);
  assert.deepEqual(column(book), ["N", "z", "r3", "r4", "r5"]);
  assert.equal(await book.sort(0, 1, false), true);
});

test("a change that fails while it is made is refused with why; the book goes on", async () => {
  const book = await rows();
  assert.match(
    await book.paste(0, { row: 2, column: 1 }, null),
    /^Not changed: /,
  );
  assert.equal(await book.paste(0, { row: 2, column: 1 }, [["ok"]]), true);
  assert.deepEqual(column(book), ["N", "ok", "r3", "r4", "r5"]);
});

// What was calculated is kept by row and cell: each stands in one place.
const once = (book) => {
  const seen = new Set();
  for (const tab of book.tabs)
    for (const row of tab.rows)
      for (const item of [row, ...row.cells]) {
        assert.ok(!seen.has(item), `${tab.name}: a row or cell twice`);
        seen.add(item);
      }
};

test("after any change a row or cell stands in one place of the tabs, and a copied tab shows values of its own", async () => {
  const book = await open([
    tab("T", [
      ["#N", "#Twice"],
      ["5", "=A2*2"],
      ["3", "=A3*2"],
    ]),
  ]);
  await book.copyTab(0);
  await book.paste(1, { row: 2, column: 1 }, [["7"]]);
  assert.deepEqual([book.shown(0, 2, 2), book.shown(1, 2, 2)], ["10", "14"]);
  once(book);
  const own = { sheet: 0, box: { top: 2, left: 1, bottom: 3, right: 2 } };
  for (const change of [
    () => book.fillDown(0, { top: 2, left: 1, bottom: 4, right: 2 }),
    () => book.paste(0, { row: 5, column: 1 }, [["=A2", "=B2"]], own),
    () => book.insertRows(0, 2, 2),
    () => book.deleteRows(0, 2, 1),
    () => book.insertColumns(0, 1, 1),
    () => book.deleteColumns(0, 1, 1),
    () => book.moveRows(0, 3, 1, 1),
    () => book.moveColumns(0, 1, 1, 1),
    () => book.sort(0, 1, true),
    () => book.style(0, own.box, "bold", "bold", new Set()),
    () => book.undo(),
    () => book.undo(),
    () => book.redo(),
    () => book.copyTab(1),
    () => book.moveTab(0, 1),
    () => book.dropTab(0),
  ]) {
    assert.equal(typeof (await change()), "boolean");
    once(book);
  }
});

test("the cells a change gives a look keep their values while it is calculated", async () => {
  const book = await open([
    tab("Costs", [
      ["#N", "#Twice"],
      ["5", "=A2*2"],
    ]),
  ]);
  const box = { top: 2, left: 1, bottom: 2, right: 2 };
  const looks = [
    book.style(0, box, "bold", "bold", new Set()),
    book.widen(0, box, 120),
    book.mark(0, "data-frozen", "1"),
  ];
  assert.deepEqual(shown(book, 0, 2, 2), ["5", "10"]);
  assert.deepEqual(await Promise.all(looks), [true, true, true]);
  assert.deepEqual(shown(book, 0, 2, 2), ["5", "10"]);
  assert.equal(book.marks(0, 2, 2).bold, true);
});

test("Undo asked for behind a refused change is taken back with it: what it would undo is gone", async () => {
  const book = await rows();
  assert.equal(
    await book.paste(0, { row: 2, column: 1 }, [["kept edit"]]),
    true,
  );
  const asked = [
    book.paste(0, { row: 3, column: 1 }, [["=XNPV(1,2)"]]),
    book.undo(),
  ];
  const [refused, undone] = await Promise.all(asked);
  assert.match(refused, /A change asked for after it was not made either\.$/);
  assert.equal(undone, false);
  assert.deepEqual(column(book), ["N", "kept edit", "r3", "r4", "r5"]);
  assert.equal(book.canRedo(), false);
});

test("rows put in one after another are each worked out once", async () => {
  let made = 0;
  const counted = {
    ...engine,
    Model: class extends engine.Model {
      constructor(...given) {
        super(...given);
        made++;
      }
    },
  };
  const book = await openBook(
    counted,
    [tab("Rows", [["#N"], ["=A1"], ["2"]])],
    false,
    calculate,
  );
  const asked = Array.from({ length: 5 }, () => book.insertRows(0, 2, 1));
  assert.deepEqual(await Promise.all(asked), Array(5).fill(true));
  assert.equal(made, 5);
});

// The engine as it is, which calculates where it is asked to anywhere
// inside: a call of the page's that did would not return here, or panic.
// Each change the page makes that reaches its engine, and its answer.
const REACHING = [
  ["sheet.insertRows(0, 2, 1)", true],
  ["sheet.deleteRows(0, 2, 1)", true],
  ["sheet.insertColumns(0, 1, 1)", true],
  ["sheet.deleteColumns(0, 1, 1)", true],
  ["sheet.moveRows(0, 1, 1, 1)", true],
  ["sheet.moveColumns(0, 1, 1, 1)", true],
  ['sheet.nameTab(0, "Other")', true],
  // What it reads is blank here: nothing moves.
  ["sheet.sort(0, 1, false)", false],
  ["sheet.fillDown(0, { top: 1, left: 1, bottom: 3, right: 2 })", true],
  [
    'sheet.paste(0, { row: 4, column: 2 }, [["=FACT(99999999999)"]], { sheet: 0, box: { top: 1, left: 2, bottom: 1, right: 2 } })',
    true,
  ],
  ["sheet.undo()", true],
  ["sheet.redo()", true],
  ["sheet.copyTab(0)", true],
  ['sheet.tokens("=FACT(99999999999)").length > 0', true],
];
// The changes made one after another in a worker, on a book whose answers
// are blank: what each answered, or nothing past the limit.
const changed = (rows, changes) =>
  ask(
    { tabs: [tab("Costs", rows)] },
    5000,
    `const blank = (tabs) =>
      tabs.map((tab) => ({
        cells: tab.rows.map((row) =>
          row.cells.map(() => ({ shown: "", kind: "text", format: "general" })),
        ),
        looks: {},
        unread: [],
      }));
    const sheet = await book.openBook(engine, request.tabs, false, async (tabs) => blank(tabs));
    const answers = [];
    ${changes.map((change) => `answers.push(await ${change});`).join("\n")}
    return answers;`,
  );

test("nothing the page asks of its engine calculates: every change answers as it would on a sheet holding formulas the engine never returns from or panics on", async () => {
  assert.deepEqual(
    await changed(
      [
        ["1", "=FACT(99999999999)"],
        ["2", "=A1+XNPV(1,2)"],
        ["3", "=Costs!A2*2"],
      ],
      REACHING.map(([change]) => change),
    ),
    REACHING.map(([, answer]) => answer),
  );
  // Each alone, where a call that calculates can only fail to return.
  for (const [change] of REACHING)
    assert.equal(
      (
        await changed(
          [
            ["1", "=FACT(99999999999)"],
            ["2", "=FACT(99999999999)"],
          ],
          [change],
        )
      )?.length,
      1,
      change,
    );
});
