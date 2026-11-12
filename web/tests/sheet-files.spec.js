// An Excel or CSV file opens in the sheet viewer, read-only; it is saved as
// a sheet to change it, and any sheet goes out as a workbook or as CSV.
import { readFileSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { test, expect, openSheet, viewerOf, writeFiles } from "./fixtures.js";

const wasm = createRequire(import.meta.url).resolve("@ironcalc/wasm-xlsx");
const engine = await import(wasm);
engine.initSync({
  module: readFileSync(wasm.replace(/wasm\.js$/, "wasm_bg.wasm")),
});
const LOCALE = ["en", "UTC", "en"];

// A workbook as Excel would write it: costs with their patterns, a second
// sheet, and a hidden one that it reads.
const workbook = () => {
  const model = new engine.Model("costs", ...LOCALE);
  model.renameSheet(0, "Costs");
  [
    ["Item", "Net", "Share"],
    ["Rent", "1200", "=B2/B$4"],
    ["Power", "90.5", "=B3/B$4"],
    ["Total", "=SUM(B2:B3)"],
  ].forEach((row, r) =>
    row.forEach((text, c) => model.setUserInput(0, r + 1, c + 1, text)),
  );
  const pattern = (column, height, format) =>
    model.updateRangeStyle(
      { sheet: 0, row: 2, column, width: 1, height },
      "num_fmt",
      format,
    );
  pattern(2, 3, "#,##0.00");
  pattern(3, 2, "0%");
  model.newSheet();
  model.renameSheet(1, "Inputs");
  model.setUserInput(1, 1, 1, "Rate");
  model.setUserInput(1, 1, 2, "=Lookup!A1");
  model.newSheet();
  model.renameSheet(2, "Lookup");
  model.setUserInput(2, 1, 1, "2");
  model.hideSheet(2);
  return model.toXlsx();
};
// The file as a sheet: what Save as sheet writes. A pattern all of a
// column's cells share is the column's.
const SHEET = `<!doctype html>
<meta charset="utf-8">
<meta name="sheet" content="1">
<title>costs</title>
<table data-sheet="Costs">
<col><col data-format="#,##0.00"><col data-format="0%">
<tr><th>Item<th>Net<th>Share
<tr><td>Rent<td>1200<td>=B2/B$4
<tr><td>Power<td>90.5<td>=B3/B$4
<tr><td>Total<td>=SUM(B2:B3)<td>
</table>
<table data-sheet="Inputs">
<tr><td>Rate<td>=Lookup!A1
</table>
<table data-sheet="Lookup">
<tr><td>2
</table>
`;

test("an Excel file opens read-only, is saved as a sheet and edited there, and goes out as a workbook and as CSV", async ({
  page: app,
  session,
  host,
}) => {
  const { files, bar, field, tabs, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    workbook(),
    { path: "books/costs.xlsx" },
  );
  const status = files.locator(".page-status .notice");
  await expect(tabs).toHaveText(["Costs", "Inputs"]);
  await expect(cell("Costs", "B4")).toHaveText("1,290.50");
  await expect(cell("Costs", "C2")).toHaveText("93%");
  await expect(files.locator(".sheet-note")).toHaveText("Read-only");
  // The hidden sheet is no tab, and is there for the cell that reads it.
  await tabs.nth(1).click();
  await expect(cell("Inputs", "B1")).toHaveText("2");
  await tabs.nth(0).click();
  // Selected and read, never changed: typing is not taken, and a command
  // says why.
  await cell("Costs", "B2").click();
  await expect(bar.locator("output")).toHaveText("Costs!B2");
  await expect(field).toHaveValue("1200");
  await expect(field).toHaveAttribute("readonly");
  await app.keyboard.type("7");
  await app.keyboard.press("Enter");
  await app.keyboard.press("Delete");
  await expect(status).toHaveText(
    "This file is read-only: save it as a sheet to change it.",
  );
  await expect(cell("Costs", "B2")).toHaveText("1,200.00");
  for (const name of ["Undo", "Chart"])
    await expect(files.getByRole("button", { name })).toHaveCount(0);

  // Saved as a sheet beside it, which opens and is edited as any.
  await files.getByRole("button", { name: "Save as sheet" }).click();
  await expect(files.getByRole("heading")).toContainText("costs.html");
  expect(await saved("books/costs.html")).toBe(SHEET);
  await expect(tabs).toHaveText(["Costs", "Inputs", "Lookup"]);
  await expect(cell("Costs", "B4")).toHaveText("1,290.50");
  await cell("Costs", "B2").click();
  await app.keyboard.type("2400");
  await app.keyboard.press("Enter");
  await expect(cell("Costs", "B4")).toHaveText("2,490.50");
  await expect
    .poll(() => saved("books/costs.html"))
    .toBe(SHEET.replace(">1200<", ">2400<"));

  // It goes out as a workbook that opens again with the same cells, and
  // its open tab as CSV, as the cells show.
  const exported = async (name) => {
    await files.getByRole("button", { name: "Export" }).click();
    const [download] = await Promise.all([
      app.waitForEvent("download"),
      app.getByRole("menuitem", { name }).click(),
    ]);
    return [
      download.suggestedFilename(),
      await readFile(await download.path()),
    ];
  };
  const [name, bytes] = await exported("Excel workbook");
  expect(name).toBe("costs.xlsx");
  const again = engine.Model.fromXlsx(
    new Uint8Array(bytes),
    "costs",
    ...LOCALE,
  );
  expect(again.getWorksheetsProperties().map((sheet) => sheet.name)).toEqual([
    "Costs",
    "Inputs",
    "Lookup",
  ]);
  expect(again.getCellContent(0, 4, 2)).toBe("=SUM(B2:B3)");
  expect(again.getFormattedCellValue(0, 4, 2)).toBe("2,490.50");
  const [csv, text] = await exported("CSV of this tab");
  expect(csv).toBe("costs.csv");
  // The mark it starts with tells Excel the text is UTF-8.
  expect(`${text}`).toBe(
    '\uFEFFItem,Net,Share\nRent,"2,400.00",96%\nPower,90.50,4%\nTotal,"2,490.50",',
  );

  // The name is taken now: saving the file as a sheet again writes nothing.
  // Until the file's page has drawn there is nothing to ask: with its
  // workbook held back, the button waits.
  let release;
  await app.route(/books\/costs\.xlsx$/, async (route) => {
    await new Promise((free) => {
      release = free;
    });
    await route.continue();
  });
  await files.getByRole("button", { name: "Back to costs.xlsx" }).click();
  await expect.poll(() => !!release).toBe(true);
  await expect(
    files.getByRole("button", { name: "Save as sheet" }),
  ).toBeDisabled();
  await expect(tabs.first()).toBeDisabled();
  release();
  await files.getByRole("button", { name: "Save as sheet" }).click();
  await expect(status).toHaveText("costs.html is already there");
  expect(await saved("books/costs.html")).toBe(
    SHEET.replace(">1200<", ">2400<"),
  );
  // A workbook is not offered as the workbook it already is.
  await files.getByRole("button", { name: "Export" }).click();
  await expect(
    app.getByRole("menuitem", { name: "CSV of this tab" }),
  ).toBeVisible();
  await expect(
    app.getByRole("menuitem", { name: "Excel workbook" }),
  ).toHaveCount(0);
});

test("a CSV file opens as one tab named after it, its cells as data, read-only, and shows its text as source", async ({
  page: app,
  session,
  host,
}) => {
  const { files, tabs, cell, stage } = await openSheet(
    app,
    session,
    host.project,
    'Item;Net;Phone\n"Rent; flat";1200;+1-555-1234\nPower;90,5;00123\n',
    { path: "data/rows [2026] of the long-named export.csv" },
  );
  // A tab's name holds no brackets and at most 31 characters.
  const name = "rows 2026 of the long-named exp";
  await expect(tabs).toHaveText([name]);
  await expect(cell(name, "A2")).toHaveText("Rent; flat");
  await expect(cell(name, "B2")).toHaveText("1200");
  // A phone number and an id are not calculated or cut.
  await expect(cell(name, "C2")).toHaveText("+1-555-1234");
  await expect(cell(name, "C3")).toHaveText("00123");
  await expect(stage.locator("tr.sheet-head")).toHaveText("1ItemNetPhone");
  await expect(files.locator(".sheet-note")).toHaveText("Read-only");
  // The grid runs on past its rows, and takes no typing there either.
  await cell(name, "E8").click();
  await app.keyboard.type("7");
  await app.keyboard.press("Enter");
  await expect(cell(name, "E8")).toHaveText("");
  // Its first row is its header: the column is filtered from there.
  await cell(name, "A1").getByRole("button").click();
  await expect(stage.getByRole("menuitem")).toHaveCount(0);
  const words = stage.getByRole("searchbox", { name: "Filter: contains" });
  await words.fill("power");
  await words.press("Enter");
  await expect(cell(name, "A2")).toBeHidden();
  await files.getByRole("button", { name: "Source" }).click();
  await expect(files.locator(".code-view")).toContainText('"Rent; flat";1200');
});

test("a file too large to save as a sheet, and one that cannot be read, say so", async ({
  page: app,
  session,
  host,
}) => {
  await writeFiles(host.project, { "data/broken.xlsx": "no workbook" });
  const { files, cell, stage, saved } = await openSheet(
    app,
    session,
    host.project,
    `Note\n${`${"word ".repeat(200)}\n`.repeat(1100)}`,
    { path: "data/notes.csv" },
  );
  await expect(cell("notes", "A1")).toHaveText("Note");
  await files.getByRole("button", { name: "Save as sheet" }).click();
  await expect(files.locator(".page-status .notice")).toHaveText(
    "Not saved: the file would be larger than 1 MB.",
  );
  await expect(saved("data/notes.html")).rejects.toThrow(/ENOENT/);

  await files.getByRole("button", { name: "Back to the list" }).click();
  await files.getByRole("button", { name: "broken.xlsx", exact: true }).click();
  await expect(stage.locator("p")).toHaveText(
    /^broken\.xlsx cannot be shown: ./,
  );
  await expect(stage.locator("table")).toHaveCount(0);
});

// The frame is untrusted: a page cannot make the app download or save.
test("a file a page hands over unasked is not taken", async ({
  page: app,
  session,
  host,
}) => {
  await app.addInitScript(() => {
    window.downloads = [];
    const { click } = HTMLAnchorElement.prototype;
    HTMLAnchorElement.prototype.click = function () {
      if (this.download) window.downloads.push(this.download);
      click.call(this);
    };
  });
  const hand = (as) =>
    `<script>parent.postMessage({ uagentDesign: 1, type: "file", as: "${as}", data: "a,b" }, "*")</script>`;
  await writeFiles(host.project, {
    "pages/plain.html": `<!doctype html><title>Plain</title>${hand("csv")}<p>Plain`,
  });
  const { files, bar } = await openSheet(
    app,
    session,
    host.project,
    `<!doctype html><meta name="sheet" content="1"><title>Plan</title>${hand("csv")}${hand("sheet")}\n<table data-sheet="One">\n<tr><td>1\n</table>\n`,
  );
  // Its selection said, the sheet's earlier words were heard too.
  await expect(bar.locator("output")).toHaveText("One!A1");
  await expect(files.locator(".page-status .notice")).toHaveCount(0);
  await files.getByRole("button", { name: "Back to the list" }).click();
  await files.getByRole("button", { name: "pages", exact: true }).click();
  await files.getByRole("button", { name: "plain.html", exact: true }).click();
  await expect(viewerOf(files).stage.getByText("Plain")).toBeVisible();
  await expect(files.locator(".page-stage iframe")).toHaveAttribute(
    "title",
    "Plain",
  );
  expect(await app.evaluate(() => window.downloads)).toEqual([]);
});
