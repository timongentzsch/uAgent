// A page's values from a CSV of its folder: filled where its marks say,
// formatted, said where they came from, and drawn again when the file is.
import { readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import {
  test,
  expect,
  artboard,
  openAlone,
  paper,
  openFiles,
  viewerOf,
  writeFiles,
} from "./fixtures.js";

const SALES =
  'month,region,revenue,note\n2027-03,EU,198700,"cold, wet"\n2027-03,US,266400,ok\n';

test("marks fill from a CSV in the page's folder, say their file, column and row, and open the file at that row", async ({
  page: app,
  session,
  host,
}) => {
  await writeFiles(join(host.project, "design/kpi"), {
    "sales.csv": SALES,
    // As a spreadsheet saves it elsewhere: semicolons, CRLF, a quoted comma.
    "umsatz.csv": 'monat;umsatz\r\n2027-03;"198,7"\r\n',
    "01-march.html": artboard(
      "March",
      '<p>DE <b id="de" data-source="umsatz.csv" data-field="umsatz"></b></p>' +
        '<p>EU <b id="eu" data-source="sales.csv" data-field="revenue" data-where="month=2027-03, region=EU" data-format="$#,##0"></b></p>' +
        '<p>US <b id="us" data-source="sales.csv" data-field="revenue" data-where="region=US"></b></p>' +
        '<p>Note <b id="note" data-source="sales.csv" data-field="note"></b></p>',
    ),
  });
  const files = await openFiles(app, session, "design/kpi/01-march.html");
  const { stage } = viewerOf(files);
  await expect(stage.locator("#eu")).toHaveText("$198,700");
  await expect(stage.locator("#us")).toHaveText("266400");
  await expect(stage.locator("#note")).toHaveText("cold, wet");
  await expect(stage.locator("#de")).toHaveText("198,7");
  // Opened alone, outside the app, the page fills the same.
  const alone = await openAlone(app, files);
  await expect(alone.locator("#eu")).toHaveText("$198,700");
  await alone.close();

  await stage.locator("#us").hover();
  const chip = files.locator(".page-source");
  await expect(chip).toContainText("sales.csv · revenue · row 2");
  await chip.hover();
  await chip.getByRole("button", { name: "Open" }).click();
  await expect(files.getByRole("heading")).toContainText("sales.csv");

  // The file written again (an agent's write reloads every page; here the
  // page is opened again), the page shows what it now says.
  await writeFile(
    join(host.project, "design/kpi/sales.csv"),
    SALES.replace("198700", "201000"),
  );
  await files.getByRole("button", { name: "Back" }).click();
  await expect(stage.locator("#eu")).toHaveText("$201,000");
});

test("a page changed in place fills its marks again, the page not loaded again", async ({
  page: app,
  session,
  host,
}) => {
  const path = join(host.project, "design/kpi/01-march.html");
  await writeFiles(join(host.project, "design/kpi"), {
    "sales.csv": SALES,
    "01-march.html": paper(
      "March",
      '<h1>March</h1><p>EU <b id="eu" data-source="sales.csv" data-field="revenue"></b></p>',
    ),
  });
  const files = await openFiles(app, session, "design/kpi/01-march.html");
  const { stage } = viewerOf(files);
  await expect(stage.locator("#eu")).toHaveText("198700");
  await stage.locator("body").evaluate(() => {
    window.kept = true;
  });
  await app.waitForResponse((response) => response.url().includes("stamps="));
  await writeFile(
    path,
    (await readFile(path, "utf8")).replace(
      "</p>",
      '</p><p>US <b id="us" data-source="sales.csv" data-field="revenue" data-where="region=US"></b></p>',
    ),
  );
  await expect(stage.locator("#us")).toHaveText("266400");
  expect(await stage.locator("body").evaluate(() => window.kept)).toBe(true);
});

test("a mark that names a missing file, column or row is said to the agent, and the page is ready after its values", async ({
  page: app,
  session,
  host,
}) => {
  await writeFiles(join(host.project, "design/kpi"), {
    "sales.csv": SALES,
    "rows.json": '[{"region":"EU","orders":1302}]',
    "01-march.html": artboard(
      "March",
      '<b id="orders" data-source="rows.json" data-field="orders" data-format="#,##0"></b>' +
        '<b data-source="sales.csv" data-field="profit"></b>' +
        '<b data-source="sales.csv" data-field="revenue" data-where="region=APAC"></b>' +
        '<b data-source="gone.csv" data-field="revenue"></b>',
    ),
  });
  const files = await openFiles(app, session, "design/kpi/01-march.html");
  const { stage } = viewerOf(files);
  await expect(stage.locator("#orders")).toHaveText("1,302");
  const said = await stage
    .locator("body")
    .evaluate(async () => await window.uagentReady);
  expect(said).toEqual([
    'sales.csv has no column "profit"',
    "sales.csv has no row where region=APAC",
    "gone.csv did not load",
  ]);
});

// The code that fills marks loads only where a page has marks, once.
const markCode = (app) => {
  const loaded = [];
  app.on("request", (request) => {
    const name = /~sheet\/(marks|text)\.js$/.exec(request.url())?.[1];
    if (name) loaded.push(name);
  });
  return loaded;
};

test("a page without marks loads no mark code", async ({
  page: app,
  session,
  host,
}) => {
  const loaded = markCode(app);
  await writeFiles(join(host.project, "plain"), {
    "plain.html": paper("Plain", "<h1>Plain</h1><p>No values from files.</p>"),
  });
  const { stage } = viewerOf(await openFiles(app, session, "plain/plain.html"));
  await expect(stage.locator("h1")).toHaveText("Plain");
  await stage.locator("body").evaluate(async () => await window.uagentReady);
  expect(loaded).toEqual([]);
});

test("a page with marks loads the mark code once", async ({
  page: app,
  session,
  host,
}) => {
  const loaded = markCode(app);
  await writeFiles(join(host.project, "marked"), {
    "sales.csv": SALES,
    "marked.html": paper(
      "Marked",
      '<p>EU <b id="eu" data-source="sales.csv" data-field="revenue"></b></p>' +
        '<p>US <b id="us" data-source="sales.csv" data-field="revenue" data-where="region=US"></b></p>',
    ),
  });
  const { stage } = viewerOf(
    await openFiles(app, session, "marked/marked.html"),
  );
  await expect(stage.locator("#us")).toHaveText("266400");
  await stage.locator("body").evaluate(async () => await window.uagentReady);
  expect(loaded.sort()).toEqual(["marks", "text"]);
});

test("where the mark code cannot load, the page draws and says its values were not filled", async ({
  page: app,
  session,
  host,
}) => {
  await app.route("**/~sheet/marks.js", (route) => route.abort());
  await writeFiles(join(host.project, "design/kpi"), {
    "rows.json": '[{"region":"EU","orders":1302}]',
    "01-march.html": artboard(
      "March",
      '<h1>Orders</h1><b id="orders" data-source="rows.json" data-field="orders"></b>',
    ),
  });
  const files = await openFiles(app, session, "design/kpi/01-march.html");
  const { stage } = viewerOf(files);
  await expect(stage.locator("h1")).toHaveText("Orders");
  const said = await stage
    .locator("body")
    .evaluate(async () => await window.uagentReady);
  expect(said).toHaveLength(1);
  expect(said[0]).toMatch(/^values from files were not filled: /);
  await expect(stage.locator("#orders")).toHaveText("");
});
