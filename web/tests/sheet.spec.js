// A sheet opens in the viewer as its own grid, calculated and drawn by the
// page in its sandboxed frame; the pane adds its tabs, its cell and its errors.
import { test, expect, openSheet, sheet, SAMPLE_SHEET } from "./fixtures.js";

const selected = (tabs) =>
  tabs.and(tabs.page().locator('[aria-selected="true"]'));

test("a sheet is calculated and drawn as a grid, with its tabs, its clicked cell and its cells in error", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, tabs, bar, field, cell } = await openSheet(
    app,
    session,
    host.project,
    SAMPLE_SHEET,
  );
  await expect(tabs).toHaveText(["Tasks", "Inputs", "Notes"]);
  await expect(selected(tabs)).toHaveText("Tasks");
  // A count, a total in its format, and under the summary the header band.
  await expect(cell("Tasks", "B1")).toHaveText("2");
  await expect(cell("Tasks", "C7")).toHaveText("9,600.50");
  const tasks = stage.locator('table[data-sheet="Tasks"]');
  await expect(tasks.locator("tr.sheet-head")).toContainText("TaskStatusCost");
  await expect(tasks.locator("tr.sheet-letters")).toHaveText(/^ABCDE/);
  // The grid is drawn in the app's palette, and follows it to another theme.
  const drawn = () =>
    stage
      .locator("html")
      .evaluate((root) => root.style.getPropertyValue("--sheet-ground"));
  const ground = () =>
    app.evaluate(() =>
      getComputedStyle(document.documentElement)
        .getPropertyValue("--bg")
        .trim(),
    );
  const light = await ground();
  await expect.poll(drawn).toBe(light);
  await app.evaluate(() => {
    document.documentElement.dataset.theme = "dark";
  });
  expect(await ground()).not.toBe(light);
  await expect
    .poll(async () => (await drawn()) === (await ground()))
    .toBe(true);
  // Its cells in error, for the agent to fix.
  const notice = files.locator(".page-notice");
  await expect(notice).toContainText("2 cells in error");
  await expect(
    notice.getByRole("button", { name: "Ask agent to fix" }),
  ).toBeVisible();
  // A grid is not stepped, picked from or presented.
  for (const name of ["Select", "Find", "Present", "Legend"])
    await expect(files.getByRole("button", { name })).toHaveCount(0);
  await expect(files.getByRole("group", { name: "Step" })).toHaveCount(0);

  // The bar says where the active cell is and what was typed in it.
  await expect(bar.locator("output")).toHaveText("Tasks!A1");
  await expect(field).toHaveValue("Done");
  await cell("Tasks", "B1").click();
  await expect(bar.locator("output")).toHaveText("Tasks!B1");
  await expect(field).toHaveValue("=COUNTIF(B4:B6,A1)");

  // The arrows and the paging keys are the grid's, in it and in the pane:
  // the tab stays. With Ctrl a paging key shows the next tab, once.
  await app.keyboard.press("ArrowRight");
  await expect(bar.locator("output")).toHaveText("Tasks!C1");
  await field.click();
  for (const key of ["ArrowRight", "ArrowDown", "PageDown", "End"])
    await app.keyboard.press(key);
  await expect(selected(tabs)).toHaveText("Tasks");
  await app.keyboard.press("Control+PageDown");
  await expect(selected(tabs)).toHaveText("Inputs");
  await expect(tasks).toBeHidden();
  // A value read from the other tab.
  await expect(cell("Inputs", "B2")).toHaveText("19,201.00");

  // Drawn again, the sheet shows the tab it showed.
  await stage.locator("html").evaluate(() => location.reload());
  await expect(cell("Inputs", "B2")).toBeVisible();
  await expect(selected(tabs)).toHaveText("Inputs");
  // The keys change the tab from inside the grid too.
  await cell("Inputs", "B2").click();
  await app.keyboard.press("Control+PageDown");
  await expect(selected(tabs)).toHaveText("Notes");
  await app.keyboard.press("Control+PageUp");
  await expect(cell("Inputs", "B2")).toBeVisible();

  // A click shows a tab; Left and Right walk the tabs from one of them, and
  // Home and End go to the first and the last.
  await tabs.nth(2).click();
  await expect(stage.getByText("Nothing yet")).toBeVisible();
  await tabs.nth(2).focus();
  await app.keyboard.press("Home");
  await expect(selected(tabs)).toHaveText("Tasks");
  await expect(tabs.nth(0)).toBeFocused();
  await app.keyboard.press("End");
  await expect(selected(tabs)).toHaveText("Notes");
  await expect(tabs.nth(2)).toBeFocused();
  await app.keyboard.press("ArrowLeft");
  await expect(selected(tabs)).toHaveText("Inputs");
  await expect(tabs.nth(1)).toBeFocused();
  await app.keyboard.press("Control+PageUp");
  await expect(selected(tabs)).toHaveText("Tasks");
  await expect(cell("Tasks", "B1")).toBeVisible();
});

test("a sheet shows nothing as written while its engine loads, and is drawn without an error of its own", async ({
  page: app,
  session,
  host,
}) => {
  // The engine is held until the test hands it over, which it does inside
  // a report of the page's size: there, a page that resizes itself is in
  // error. An observer's first report always comes.
  await app.addInitScript(() => {
    if (!location.pathname.startsWith("/preview/")) return;
    let release;
    const instantiate = WebAssembly.instantiateStreaming;
    const held = new Promise((reached) => {
      WebAssembly.instantiateStreaming = async (...given) => {
        const made = await instantiate(...given);
        await new Promise((done) => {
          release = done;
          reached();
        });
        return made;
      };
    });
    window.handOver = async () => {
      await held;
      new ResizeObserver((_, watching) => {
        watching.disconnect();
        release();
      }).observe(document.body);
    };
  });
  const { files, stage, cell } = await openSheet(
    app,
    session,
    host.project,
    SAMPLE_SHEET,
  );
  await expect(stage.locator("table[data-sheet]").first()).toHaveCSS(
    "visibility",
    "hidden",
  );
  // Until it has drawn, the stage says it is opening.
  const opening = files.getByRole("status", { name: "Opening the page…" });
  await expect(opening).toBeVisible();
  await stage.locator("html").evaluate(() => window.handOver());
  await expect(cell("Tasks", "B1")).toHaveText("2");
  await expect(opening).toHaveCount(0);
  await expect(files.locator(".page-notice")).toContainText(
    /^2 cells in error/,
  );
});

test("on a phone a sheet's tabs scroll sideways and its formula bar stays one line", async ({
  page: app,
  session,
  host,
}) => {
  const long = `=${Array(30).fill("A1").join("+")}`;
  const { files, tabs, bar, field, cell } = await openSheet(
    app,
    session,
    host.project,
    sheet(
      "Year",
      Object.fromEntries(
        Array.from({ length: 12 }, (_, at) => [
          `Month ${at + 1}`,
          [`<td>1<td>${long}`],
        ]),
      ),
    ),
    { phone: true },
  );
  await expect(tabs.last()).toHaveText("Month 12");
  const list = files.locator(".sheet-tabs");
  const row = await tabs.first().evaluate((tab) => tab.offsetHeight);
  expect(
    await list.evaluate((box) => [
      box.scrollWidth > box.clientWidth,
      box.clientHeight,
    ]),
  ).toEqual([true, expect.any(Number)]);
  expect(await list.evaluate((box) => box.clientHeight)).toBeLessThan(2 * row);
  // The last tab is out of view until it is shown: the row follows.
  await expect(tabs.last()).not.toBeInViewport();
  await field.click();
  for (let at = 1; at < 12; at++) await app.keyboard.press("Control+PageDown");
  await expect(selected(tabs)).toHaveText("Month 12");
  await expect(tabs.last()).toBeInViewport();

  // A long formula is cut short after its cell's name, which stays whole.
  const short = await bar.evaluate((box) => box.offsetHeight);
  await cell("Month 12", "B1").click();
  await expect(bar.locator("output")).toHaveText("Month 12!B1");
  expect(await bar.evaluate((box) => box.offsetHeight)).toBe(short);
  expect(
    await bar.evaluate((box) => {
      const ref = box.querySelector("output");
      const text = box.querySelector("input");
      return [
        ref.scrollWidth <= ref.clientWidth,
        text.scrollWidth > text.clientWidth,
      ];
    }),
  ).toEqual([true, true]);
});

test("a sheet is drawn once its stylesheet has loaded, however late that comes", async ({
  page: app,
  session,
  host,
}) => {
  // The stylesheet comes after the engine has.
  const engine = app.waitForEvent("requestfinished", (request) =>
    request.url().endsWith("wasm_bg.wasm"),
  );
  await app.route("**/~sheet/sheet.css", async (route) => {
    await engine;
    await route.continue();
  });
  // In the page: whether the stylesheet was in when its first rows were drawn.
  await app.addInitScript(() => {
    new MutationObserver((_, observer) => {
      if (!document.querySelector("tbody [data-row]")) return;
      observer.disconnect();
      window.styled = !!document.querySelector('link[href$="sheet.css"]').sheet;
    }).observe(document, { subtree: true, childList: true });
  });
  const { stage, cell } = await openSheet(
    app,
    session,
    host.project,
    SAMPLE_SHEET,
  );
  await expect(cell("Tasks", "A4")).toHaveText("Write the plan");
  expect(await stage.locator("html").evaluate(() => window.styled)).toBe(true);
});

// The engine panics on XNPV of two plain numbers: the workbook it was
// calculating is lost to it.
const COSTS = sheet("Costs", {
  Costs: ["<th>Item<th>Cost<th>Twice", "<td>Rent<td>1200<td>=B2*2"],
});

test("a formula the engine panics on is refused when entered: the sheet goes on and the file stays", async ({
  page: app,
  session,
  host,
}) => {
  const { files, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    COSTS,
  );
  await cell("Costs", "B2").click();
  await app.keyboard.type("=XNPV(1,1)");
  await app.keyboard.press("Enter");
  await expect(files.locator(".page-tools .notice")).toHaveText(
    "Not changed: the engine cannot calculate a formula this would leave in the sheet.",
  );
  await expect(cell("Costs", "B2")).toHaveText("1200");
  expect(await saved()).toBe(COSTS);
  // What it takes is calculated and saved as ever, and undone.
  await cell("Costs", "B2").click();
  await app.keyboard.type("50");
  await app.keyboard.press("Enter");
  await expect(cell("Costs", "C2")).toHaveText("100");
  await expect.poll(saved).toContain("<td>50<td>=B2*2");
  await files.getByRole("button", { name: "Undo" }).click();
  await expect(cell("Costs", "C2")).toHaveText("2400");
  await expect.poll(saved).toBe(COSTS);
});

test("a file with a formula the engine panics on is shown as written, with the reason", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage } = await openSheet(
    app,
    session,
    host.project,
    sheet("Costs", {
      Costs: ["<th>Item<th>Cost", "<td>Rent<td>=XNPV(1,1)", "<td>Power<td>90"],
    }),
  );
  const reason =
    "This sheet is shown as written: the engine cannot calculate it";
  await expect(stage.locator(".sheet-notice")).toHaveText(reason);
  await expect(stage.locator("table[data-sheet] td").nth(1)).toHaveText(
    "=XNPV(1,1)",
  );
  await files
    .locator(".page-notice")
    .getByRole("button", { name: "Ask agent to fix" })
    .click();
  await expect(app.locator("#prompt")).toHaveValue(new RegExp(reason));
});
