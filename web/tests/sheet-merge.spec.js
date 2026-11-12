// Merged cells: a range the table lists is drawn as its top left cell over
// the range, which a person selects, merges and unmerges as one.
import { test, expect, openSheet, sheet } from "./fixtures.js";

// A title over the table and the halves over their quarters, under held
// rows and a held column that cut through the title; a label down rows far
// past the first screenful.
const BUDGET = sheet("Budget", {
  Costs: [
    "<td>Budget 2027<td><td><td><td>",
    "<th><th>H1<th><th>H2<th>",
    "<th>Item<th>Q1<th>Q2<th>Q3<th>Q4",
    ...Array.from(
      { length: 200 },
      (_, at) =>
        `<td>${at ? "" : "Running costs"}<td>${at + 1}<td>${at + 2}<td>${at + 3}<td>${at + 4}`,
    ),
  ],
}).replace(
  'data-sheet="Costs"',
  'data-sheet="Costs" data-frozen="3 A" data-merge="A1:E1 B2:C2 D2:E2 A4:A203"',
);
// Each drawn part of a cell: its text and how many rows and columns it
// spans.
const parts = (cell) =>
  cell.evaluateAll((drawn) =>
    drawn.map((part) => ({
      text: part.firstChild?.textContent ?? "",
      spans: [part.rowSpan, part.colSpan],
    })),
  );

test("a merged range is drawn as its top left cell, in a part on each side of what is held, wherever it is scrolled", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, cell } = await openSheet(app, session, host.project, BUDGET);
  // The held column cuts the title: its text over A, and again over B:E.
  await expect(cell("Costs", "A1")).toHaveCount(2);
  expect(await parts(cell("Costs", "A1"))).toEqual([
    { text: "Budget 2027", spans: [1, 1] },
    { text: "Budget 2027", spans: [1, 4] },
  ]);
  expect(await parts(cell("Costs", "B2"))).toEqual([
    { text: "H1", spans: [1, 2] },
  ]);
  for (const covered of ["B1", "C2", "E2", "A5"])
    await expect(cell("Costs", covered)).toHaveCount(0);
  const width = (ref) =>
    cell("Costs", ref)
      .last()
      .evaluate((part) => part.getBoundingClientRect().width);
  const columns = await stage
    .locator('table[data-sheet="Costs"] thead [data-column]')
    .evaluateAll((letters) =>
      letters.map((letter) => letter.getBoundingClientRect().width),
    );
  expect(await width("B2")).toBeCloseTo(columns[1] + columns[2], 0);
  // Scrolled far past its first row the label still shows, over the rows
  // drawn: its own row is no longer.
  await stage.locator("html").evaluate(async () => {
    for (let frames = 0; frames < 200 && scrollY < 4000; frames++) {
      scrollTo(0, 4000);
      await new Promise(requestAnimationFrame);
    }
  });
  await expect(cell("Costs", "B172")).toBeInViewport();
  await expect(cell("Costs", "A4")).toHaveText("Running costs");
  const [label] = await parts(cell("Costs", "A4"));
  expect(label.spans[0]).toBeGreaterThan(1);
  expect(label.spans[0]).toBeLessThan(200);
});

test("a merged cell is selected as one: pressed anywhere, stepped into and out of, and grown into by a range; a column's letter picks its column alone", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, bar, cell } = await openSheet(
    app,
    session,
    host.project,
    BUDGET,
  );
  const at = bar.locator("output");
  // The title's part past the held column is the title.
  await cell("Costs", "A1").last().click();
  await expect(at).toHaveText("Costs!A1");
  for (const [key, ref] of [
    ["ArrowDown", "A2"],
    ["ArrowRight", "B2"],
    ["ArrowRight", "D2"],
    ["ArrowRight", "F2"],
    ["ArrowLeft", "D2"],
    ["ArrowUp", "A1"],
    ["ArrowDown", "A2"],
    ["End", "D2"],
  ]) {
    await app.keyboard.press(key);
    await expect(at).toHaveText(`Costs!${ref}`);
  }
  // A range that cuts a merged cell takes it whole.
  await cell("Costs", "B3").click();
  await app.keyboard.press("Shift+ArrowUp");
  await expect(stage.locator(".sheet-in")).toHaveText(["H1", "Q1", "Q2"]);
  await expect(at).toHaveText("Costs!B3");
  // A column's letter picks the column, through the cells merged over it.
  await stage.locator('.sheet-letters [data-column="3"]').click();
  await app.keyboard.press("Escape");
  await expect(at).toHaveText("Costs!C1");
  await expect(cell("Costs", "C3")).toHaveClass(/sheet-in/);
  await expect(cell("Costs", "B2")).not.toHaveClass(/sheet-in/);
});
