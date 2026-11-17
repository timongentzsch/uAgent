// A page's values from a CSV of its folder: filled where its marks say,
// formatted, said where they came from, and drawn again when the file is.
import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import {
  test,
  expect,
  artboard,
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
    "01-march.html": artboard(
      "March",
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

  await stage.locator("#us").hover();
  const chip = stage.locator(".uagent-source");
  await expect(chip).toContainText("sales.csv · revenue · row 2");
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
