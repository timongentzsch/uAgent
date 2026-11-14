// Groups: rows or columns a person folds and opens from the gutter beside
// the row numbers and over the letters, made and taken out from the menus.
import { readFileSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { test, expect, openSheet, sheet } from "./fixtures.js";

const wasm = createRequire(import.meta.url).resolve("@ironcalc/wasm-xlsx");
const engine = await import(wasm);
engine.initSync({
  module: readFileSync(wasm.replace(/wasm\.js$/, "wasm_bg.wasm")),
});

// A budget whose items are grouped under their sums, both kinds in one
// group, and its amounts in a group of columns.
const BUDGET = (attrs) =>
  sheet("Budget", {
    Costs: [
      "<th>Item<th>Cost<th>Plan<th>Note",
      "<td>Rent<td>1200<td>1200<td>",
      "<td>Power<td>90<td>100<td>",
      "<td>Housing<td>=SUM(B2:B3)<td>=SUM(C2:C3)<td>",
      "<td>Bus<td>60<td>50<td>",
      "<td>Train<td>40<td>40<td>",
      "<td>Travel<td>=SUM(B5:B6)<td>=SUM(C5:C6)<td>",
      '<tr class="total"><td>Total<td>=B4+B7<td>=C4+C7<td>',
    ],
  }).replace('data-sheet="Costs"', `data-sheet="Costs" ${attrs}`);
const OUTLINE = 'data-group="2:7 2:3 5:6 B:C" data-folded="5:6"';

test("the gutter draws each group's line and its button on the line after it, nested; a button folds and opens, Undo takes it back, and the file keeps the folds", async ({
  page: app,
  session,
  host,
}) => {
  const source = BUDGET(OUTLINE);
  const { files, stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    source,
  );
  const fold = (name) => stage.getByRole("button", { name, exact: true });
  const undo = files.getByRole("button", { name: "Undo" });
  // A folded group's rows are not drawn; its button stands after them.
  await expect(cell("Costs", "A4")).toHaveText("Housing");
  for (const gone of ["A5", "A6"])
    await expect(cell("Costs", gone)).toHaveCount(0);
  const button = (name, line) =>
    stage.locator(`[data-${line}] .sheet-fold[aria-label="${name}"]`);
  await expect(button("Rows 2 to 3", 'row="4"')).toHaveAttribute(
    "aria-expanded",
    "true",
  );
  await expect(button("Rows 5 to 6", 'row="7"')).toHaveAttribute(
    "aria-expanded",
    "false",
  );
  await expect(button("Rows 5 to 6", 'row="7"')).toHaveText("+");
  await expect(button("Rows 2 to 7", 'row="8"')).toBeVisible();
  await expect(button("Columns B to C", 'column="4"')).toBeVisible();
  // Two levels of lanes beside the numbers, one over the letters.
  await expect(stage.locator('tbody [data-row="2"] .sheet-lane')).toHaveCount(
    2,
  );
  await expect(stage.locator('tbody [data-row="2"] .sheet-along')).toHaveCount(
    2,
  );
  await expect(
    stage.locator('.sheet-letters [data-column="2"] .sheet-along'),
  ).toHaveCount(1);
  // The button and the numbers it stands beside do not overlap.
  const box = (located) => located.evaluate((at) => at.getBoundingClientRect());
  const number = await box(stage.locator('tbody th[data-row="4"]'));
  const pressed = await box(button("Rows 2 to 3", 'row="4"'));
  expect(pressed.right).toBeLessThanOrEqual(number.left + 32 + 1);

  // Pressed, a button folds its group and selects nothing.
  await fold("Rows 2 to 3").click();
  await expect(cell("Costs", "A2")).toHaveCount(0);
  await expect(fold("Rows 2 to 3")).toHaveAttribute("aria-expanded", "false");
  await expect
    .poll(saved)
    .toBe(source.replace('data-folded="5:6"', 'data-folded="5:6 2:3"'));
  // The outer group folded hides the groups in it, and their buttons.
  await fold("Rows 2 to 7").click();
  await expect(cell("Costs", "A4")).toHaveCount(0);
  await expect(fold("Rows 2 to 3")).toHaveCount(0);
  await expect(fold("Rows 5 to 6")).toHaveCount(0);
  await expect(cell("Costs", "A8")).toHaveText("Total");
  await undo.click();
  await undo.click();
  await expect.poll(saved).toBe(source);
  await expect(cell("Costs", "A2")).toHaveText("Rent");
  await fold("Rows 5 to 6").click();
  await expect(cell("Costs", "A5")).toHaveText("Bus");
  await fold("Columns B to C").click();
  await expect(cell("Costs", "B1")).toHaveCount(0);
  await expect(stage.locator('.sheet-letters [data-column="2"]')).toHaveCount(
    0,
  );
  const folded = BUDGET('data-group="2:7 2:3 5:6 B:C" data-folded="B:C"');
  await expect.poll(saved).toBe(folded);

  // Drawn again, the sheet shows its folds as it kept them.
  await stage.locator("html").evaluate(() => location.reload());
  await expect(cell("Costs", "A5")).toHaveText("Bus");
  await expect(cell("Costs", "C2")).toHaveCount(0);
  await expect(cell("Costs", "D2")).toHaveCount(1);

  // It goes out to Excel with the folded columns hidden.
  await files.getByRole("button", { name: "Export" }).click();
  const [download] = await Promise.all([
    app.waitForEvent("download"),
    app.getByRole("menuitem", { name: "Excel workbook" }).click(),
  ]);
  const model = engine.Model.fromXlsx(
    new Uint8Array(await readFile(await download.path())),
    "plan",
    "en",
    "UTC",
    "en",
  );
  expect(
    [1, 2, 3, 4].map((column) => model.getColumnWidth(0, column) > 0),
  ).toEqual([true, false, false, true]);
});

test("rows are grouped, folded, opened and ungrouped from the menus, by keyboard too; one that would touch another is refused, saying why", async ({
  page: app,
  session,
  host,
}) => {
  const source = BUDGET('data-group="2:7 2:3"');
  const { files, stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    source,
  );
  const item = (name) => stage.getByRole("menuitem", { name, exact: true });
  const number = (row) => stage.locator(`tbody th[data-row="${row}"]`);
  // Rows 5 and 6 picked by their numbers, the menu asked for on them.
  await number(5).click();
  await app.keyboard.press("Escape");
  await app.keyboard.press("Shift+ArrowDown");
  await number(5).click({ button: "right" });
  await item("Group rows 5–6").click();
  await expect.poll(saved).toBe(source.replace('"2:7 2:3"', '"2:7 2:3 5:6"'));
  await expect(
    stage.locator('[data-row="7"] .sheet-fold[aria-label="Rows 5 to 6"]'),
  ).toBeVisible();
  // From a cell in it, by keyboard, the innermost group is folded.
  await cell("Costs", "A5").click();
  await app.keyboard.press("Shift+F10");
  await item("Fold rows 5–6").click();
  await expect(cell("Costs", "A5")).toHaveCount(0);
  await expect(
    files.getByRole("group", { name: "Formula bar" }).locator("output"),
  ).toHaveText("Costs!A7");
  // On the row its button stands on, it is opened.
  await app.keyboard.press("Shift+F10");
  await item("Open rows 5–6").click();
  await expect(cell("Costs", "A5")).toHaveText("Bus");
  await number(5).click({ button: "right" });
  await item("Ungroup rows 5–6").click();
  await expect.poll(saved).toBe(source);
  // Row 4 holds the button of 2:3: grouped, it would touch it.
  await number(4).click();
  await item("Group row 4").click();
  await expect(
    files.locator(".page-tools .notice, .open-page > .page-notice .notice"),
  ).toHaveText("Not changed: group `4:4` touches 2:3: leave a row between.");
  expect(await saved()).toBe(source);
});

test("a group stays openable where a filter hides the row after it, past held rows, and after the table's last row, which the file does not keep", async ({
  page: app,
  session,
  host,
}) => {
  const source = BUDGET('data-frozen="3" data-group="2:3 8:8"');
  const { stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    source,
  );
  const fold = (name) => stage.getByRole("button", { name, exact: true });
  const on = (row, name) =>
    stage.locator(`[data-row="${row}"] .sheet-fold[aria-label="${name}"]`);
  // Folded, the held rows 2 and 3 are not drawn; the button is on row 4.
  await fold("Rows 2 to 3").click();
  await expect(cell("Costs", "A2")).toHaveCount(0);
  await expect(on(4, "Rows 2 to 3")).toBeVisible();
  // A filter hides row 4: the button goes on to the next row shown.
  await cell("Costs", "A4").click({ button: "right" });
  await stage.getByRole("menuitem", { name: "Leave out “Housing”" }).click();
  await expect(cell("Costs", "A4")).toHaveCount(0);
  await expect(on(5, "Rows 2 to 3")).toBeVisible();
  await on(5, "Rows 2 to 3").click();
  await expect(cell("Costs", "A2")).toHaveText("Rent");
  await expect(stage.locator('tbody tr:has([data-row="2"])')).toHaveClass(
    /sheet-frozen/,
  );
  // The total row's group has its button on the first row past the table.
  await expect(on(9, "Row 8")).toBeVisible();
  await on(9, "Row 8").click();
  await expect(cell("Costs", "A8")).toHaveCount(0);
  await expect
    .poll(saved)
    .toBe(
      source.replace(
        'data-group="2:3 8:8"',
        'data-group="2:3 8:8" data-folded="8:8"',
      ),
    );
  await on(9, "Row 8").click();
  await expect(cell("Costs", "A8")).toHaveText("Total");
});
