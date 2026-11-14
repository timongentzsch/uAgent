// A formula being typed, in a cell or in the formula bar, is offered the
// functions its letters name and told the arguments of the one it stands in.
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { test, expect, openSheet, sheet } from "./fixtures.js";

const wasm = createRequire(import.meta.url).resolve("@ironcalc/wasm-xlsx");
const engine = await import(wasm);
engine.initSync({
  module: readFileSync(wasm.replace(/wasm\.js$/, "wasm_bg.wasm")),
});

const COSTS = sheet("Costs", {
  Costs: [
    "<th>Item<th>Cost",
    "<td>Rent<td>1200",
    "<td>Power<td>90.5",
    "<td>Water<td>40",
  ],
});
const options = (list) => list.getByRole("option");
const picked = (list) => list.locator('[aria-selected="true"]');

test("letters typed after = in a cell list the functions they name; the keys walk the list and take from it, and the call's arguments follow the caret", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    COSTS,
  );
  const editor = stage.getByRole("textbox", { name: "Cell" });
  const list = stage.getByRole("listbox", { name: "Functions" });
  const hint = stage.getByRole("note");
  await cell("Costs", "D2").click();
  // Those that start with the letters, then those that hold them.
  await app.keyboard.type("=sum");
  await expect(options(list)).toHaveText([
    /^SUM Numbers added$/,
    /^SUMIF /,
    /^SUMIFS /,
    /^SUMPRODUCT /,
    /^SUMSQ /,
    /^SUMX2MY2 /,
    /^SUMX2PY2 /,
    /^SUMXMY2 /,
    /^DSUM /,
    /^IMSUM /,
    /^SERIESSUM /,
  ]);
  // The first is the one the keys are at; the arrows go round, and the one
  // they are at is in view.
  await expect(picked(list)).toHaveText(/^SUM /);
  await app.keyboard.press("ArrowUp");
  await expect(picked(list)).toHaveText(/^SERIESSUM /);
  await expect(picked(list)).toBeInViewport({ ratio: 1 });
  const within = await picked(list).evaluate((row) => {
    const [own, all] = [row, row.parentElement].map((box) =>
      box.getBoundingClientRect(),
    );
    return own.top >= all.top && own.bottom <= all.bottom;
  });
  expect(within).toBe(true);
  await app.keyboard.press("ArrowDown");
  await app.keyboard.press("ArrowDown");
  await expect(picked(list)).toHaveText(/^SUMIF /);
  // Enter takes it: its name and a bracket stand for the letters, and its
  // arguments are said with the first one marked.
  await app.keyboard.press("Enter");
  await expect(editor).toHaveValue("=SUMIF(");
  await expect(list).toBeHidden();
  await expect(hint).toHaveText("SUMIF(range, criteria, [sum_range])");
  await expect(hint.locator("b")).toHaveText("range");
  await app.keyboard.type("B2:B4,");
  await expect(hint.locator("b")).toHaveText("criteria");
  // The caret moved back before the comma is at the first again.
  await app.keyboard.press("ArrowLeft");
  await expect(hint.locator("b")).toHaveText("range");
  await app.keyboard.press("ArrowRight");
  await expect(hint.locator("b")).toHaveText("criteria");
  // A call closed says no more, and Enter writes the cell.
  await app.keyboard.type('">100")');
  await expect(hint).toBeHidden();
  await app.keyboard.press("Enter");
  await expect(cell("Costs", "D2")).toHaveText("1200");
  await expect.poll(saved).toContain('<td>=SUMIF(B2:B4,"&gt;100")');

  // After an operator and inside a call's brackets too; Tab takes as Enter
  // does, and the call the caret is in is the inner one.
  await app.keyboard.type("=1+round(max");
  await expect(picked(list)).toHaveText(/^MAX /);
  await app.keyboard.press("Tab");
  await expect(editor).toHaveValue("=1+round(MAX(");
  await expect(hint.locator("b")).toHaveText("number1");
  await app.keyboard.type("B2:B4),");
  await expect(hint).toHaveText("ROUND(number, num_digits)");
  await expect(hint.locator("b")).toHaveText("num_digits");
  // With no list open Tab writes the cell and moves right, as it did.
  await app.keyboard.type("-2)");
  await app.keyboard.press("Tab");
  await expect(cell("Costs", "D3")).toHaveText("1201");
  await expect(cell("Costs", "E3")).toHaveAttribute("aria-selected", "true");

  // Escape shuts the list and the typing goes on; a second leaves the cell
  // as it was.
  await app.keyboard.type("=ab");
  await expect(picked(list)).toHaveText(/^ABS /);
  await app.keyboard.press("Escape");
  await expect(list).toBeHidden();
  await expect(editor).toHaveValue("=ab");
  await app.keyboard.type("s");
  await expect(picked(list)).toHaveText(/^ABS /);
  await app.keyboard.press("Escape");
  await app.keyboard.press("Escape");
  await expect(editor).not.toHaveAttribute("data-editing");
  await expect(cell("Costs", "E3")).toHaveText("");

  // A press takes one too, and the cell is still being typed in.
  await app.keyboard.type("=av");
  await options(list)
    .filter({ hasText: /^AVERAGE / })
    .click();
  await expect(editor).toHaveValue("=AVERAGE(");
  await expect(editor).toBeFocused();
  await app.keyboard.type("B2:B4)");
  await app.keyboard.press("Enter");
  await expect(cell("Costs", "E3")).toHaveText("443.5");
  // What is no formula is offered nothing, and Enter writes it.
  await app.keyboard.type("sum");
  await expect(editor).toHaveValue("sum");
  await expect(list).toBeHidden();
  await app.keyboard.press("Enter");
  await expect(cell("Costs", "E4")).toHaveText("sum");
  // A bracket that already follows the caret is the name's; and a number's
  // letter names no function.
  await app.keyboard.type("=su(B2:B4)");
  for (const _ of "(B2:B4)") await app.keyboard.press("ArrowLeft");
  await app.keyboard.type("m");
  await expect(picked(list)).toHaveText(/^SUM /);
  await app.keyboard.press("Enter");
  await expect(editor).toHaveValue("=SUM(B2:B4)");
  await app.keyboard.press("Enter");
  await expect(cell("Costs", "E5")).toHaveText("1330.5");
  await app.keyboard.type("=1e");
  await expect(list).toBeHidden();
  await app.keyboard.press("Escape");
});

test("the formula bar lists the functions under its field and says a call's arguments; without a list Enter writes and Tab moves on", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, bar, field, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    COSTS,
  );
  const list = bar.getByRole("listbox", { name: "Functions" });
  const hint = bar.getByRole("note");
  const editor = stage.getByRole("textbox", { name: "Cell" });
  await cell("Costs", "B5").click();
  await field.click();
  await field.pressSequentially("=ave");
  // Typed in the cell too, but the list is where the caret is.
  await expect(editor).toHaveValue("=ave");
  await expect(stage.getByRole("listbox")).toHaveCount(0);
  await expect(options(list)).toHaveText([
    /^AVEDEV/,
    /^AVERAGE/,
    /^AVERAGEA/,
    /^AVERAGEIF/,
    /^AVERAGEIFS/,
    /^DAVERAGE/,
  ]);
  await expect(picked(list)).toHaveText(/^AVEDEV/);
  // Under the field, over the grid.
  const [typed, offered] = await Promise.all([
    field.boundingBox(),
    list.boundingBox(),
  ]);
  expect(offered.y).toBeGreaterThanOrEqual(typed.y + typed.height - 1);
  await field.press("ArrowDown");
  await expect(picked(list)).toHaveText("AVERAGEThe mean of numbers");
  await expect(field).toHaveAttribute(
    "aria-activedescendant",
    await picked(list).getAttribute("id"),
  );
  await field.press("Enter");
  await expect(field).toHaveValue("=AVERAGE(");
  await expect(list).toBeHidden();
  await expect(editor).toHaveValue("=AVERAGE(");
  await expect(stage.getByRole("note")).toHaveCount(0);
  await expect(hint).toHaveText("AVERAGE(number1, [number2], …)");
  await expect(hint.locator("strong")).toHaveText("number1");
  // Past the arguments named, those that may follow.
  await field.pressSequentially("B2,B3,B4");
  await expect(hint.locator("strong")).toHaveText("…");
  // The caret moved back is at the argument it stands in.
  for (const _ of ",B4") await field.press("ArrowLeft");
  await expect(hint.locator("strong")).toHaveText("[number2]");
  for (const _ of ",B4") await field.press("ArrowRight");
  await expect(hint.locator("strong")).toHaveText("…");
  await field.pressSequentially(")");
  await expect(hint).toBeHidden();
  await field.press("Enter");
  await expect(cell("Costs", "B5")).toHaveText("443.5");
  await expect.poll(saved).toContain("<td>=AVERAGE(B2,B3,B4)");

  // Escape shuts the list and keeps what was typed; again, it puts back
  // what the cell holds.
  await field.pressSequentially("+mi");
  await expect(picked(list)).toHaveText(/^MID/);
  await field.press("Escape");
  await expect(list).toBeHidden();
  await expect(field).toHaveValue("=AVERAGE(B2,B3,B4)+mi");
  await field.pressSequentially("n");
  // A press takes one, and the field keeps the focus.
  await list.locator("strong", { hasText: /^MIN$/ }).click();
  await expect(field).toHaveValue("=AVERAGE(B2,B3,B4)+MIN(");
  await expect(field).toBeFocused();
  await expect(hint.locator("strong")).toHaveText("number1");
  await field.press("Escape");
  await expect(field).toHaveValue("=AVERAGE(B2,B3,B4)");
  // Tab is the field's own where no list is open: the focus moves on.
  await field.pressSequentially("+1");
  await field.press("Tab");
  await expect(field).not.toBeFocused();
  await expect(files.getByRole("button", { name: "Bold" })).toBeFocused();

  // A function taken is typed as the letters were: left at once, the sheet
  // has it in its file.
  await cell("Costs", "C5").click();
  await app.evaluate(() =>
    addEventListener("message", ({ data }) => {
      if (data?.type === "typing") window.said = data.text;
    }),
  );
  await field.click();
  await field.pressSequentially("=sum");
  await expect(picked(list)).toHaveText("SUMNumbers added");
  await field.press("Enter");
  await field.pressSequentially("B2:B4)");
  await expect.poll(() => app.evaluate(() => window.said)).toBe("=SUM(B2:B4)");
  await files.getByRole("button", { name: /^Back to/ }).click();
  await expect.poll(saved).toContain("<td>=SUM(B2:B4)");
});

test("a file that is only read is offered nothing", async ({
  page: app,
  session,
  host,
}) => {
  const model = new engine.Model("costs", "en", "UTC", "en");
  model.setUserInput(0, 1, 1, "=SUM(1,2)");
  const { files, stage, bar, field, cell } = await openSheet(
    app,
    session,
    host.project,
    model.toXlsx(),
    { path: "data/costs.xlsx" },
  );
  await expect(cell("Sheet1", "A1")).toHaveText("3");
  await cell("Sheet1", "B1").click();
  await app.keyboard.type("=su");
  await expect(stage.getByRole("textbox", { name: "Cell" })).toHaveValue(" ");
  await expect(stage.locator(".sheet-suggest")).toHaveCount(0);
  // In the bar the caret stands in a call, and the page is not asked: by
  // the time it says the next cell selected, it would have answered.
  await app.evaluate(() =>
    addEventListener("message", ({ data }) => {
      if (data?.type === "suggest") window.offered = true;
    }),
  );
  await cell("Sheet1", "A1").click();
  await expect(field).toHaveValue("=SUM(1,2)");
  await field.click();
  await field.press("ArrowLeft");
  await cell("Sheet1", "B1").click();
  await expect(bar.locator("output")).toHaveText("Sheet1!B1");
  expect(await app.evaluate(() => window.offered)).toBeUndefined();
});

test("on a phone the list lies clear of the cell typed in, and under the formula bar's field", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, bar, field, cell } = await openSheet(
    app,
    session,
    host.project,
    sheet("Rows", {
      Rows: Array.from({ length: 60 }, (_, row) => `<td>${row}<td>`),
    }),
    { phone: true },
  );
  const editor = stage.getByRole("textbox", { name: "Cell" });
  const list = stage.getByRole("listbox", { name: "Functions" });
  const boxes = () =>
    Promise.all([editor.boundingBox(), list.boundingBox()]).then(
      ([cell, list]) => ({ cell, list }),
    );
  // In the first row the list is under the cell.
  await cell("Rows", "B1").dblclick();
  await app.keyboard.type("=s");
  await expect(list).toBeInViewport({ ratio: 1 });
  const first = await boxes();
  expect(first.list.y).toBeGreaterThanOrEqual(
    first.cell.y + first.cell.height - 1,
  );
  await app.keyboard.press("Escape");
  await app.keyboard.press("Escape");
  // In the last row in view there is no room under it: over the cell.
  const rows = await stage.locator("html").evaluate(() => {
    const high = document
      .querySelector("tbody [data-row='2']")
      .getBoundingClientRect().height;
    return Math.floor(innerHeight / high) - 1;
  });
  await cell("Rows", `B${rows}`).dblclick();
  await app.keyboard.type("=s");
  await expect(list).toBeInViewport({ ratio: 1 });
  const last = await boxes();
  expect(last.list.y + last.list.height).toBeLessThanOrEqual(last.cell.y + 1);
  await app.keyboard.press("Escape");
  await app.keyboard.press("Escape");
  // From Edit cell, the bar's list is under its field and in the window.
  await files.getByRole("button", { name: "Edit cell" }).click();
  await field.pressSequentially("=s");
  const offered = bar.getByRole("listbox", { name: "Functions" });
  await expect(offered).toBeInViewport({ ratio: 1 });
  const [typed, under] = await Promise.all([
    field.boundingBox(),
    offered.boundingBox(),
  ]);
  expect(under.y).toBeGreaterThanOrEqual(typed.y + typed.height - 1);
});
