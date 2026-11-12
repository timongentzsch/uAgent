// A sheet is edited in the pane as in a spreadsheet, and every change is
// saved into its file: the tab's table, in the form the file keeps one.
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import {
  test,
  expect,
  openSheet,
  selectProbeModel,
  sheet,
  writeFiles,
} from "./fixtures.js";

// Tasks with a list of statuses, a box to tick, costs and what doubles them,
// under one header row and over a total; a second tab reads the total.
const PLAN = sheet("Plan", {
  Tasks: [
    '<col><col data-options="To do|In progress|Done"><col data-control="checkbox"><col data-format="#,##0.00"><col>',
    "<th>Task<th>Status<th>Done<th>Cost<th>Double",
    "<td>Write<td>Done<td>TRUE<td>1200<td>=D2*2",
    "<td>Book<td>To do<td>FALSE<td>8400.5<td>=D3*2",
    "<td>Brief<td>In progress<td>FALSE<td>300<td>=D4*2",
    '<tr class="total"><td>Total<td><td>=COUNTIF(C2:C4,TRUE)<td>=SUM(D2:D4)<td>=SUM(E2:E4)',
  ],
  Inputs: ["<th>Name<th>Value", "<td>Budget<td>=Tasks!D5"],
});
// The file with some of its text another.
const with_ = (source, changes) =>
  Object.entries(changes).reduce((text, [from, to]) => {
    expect(text).toContain(from);
    return text.replace(from, to);
  }, source);

test("what is typed into a cell or the formula bar is saved to the file, and undo and redo take it back and forth", async ({
  page: app,
  session,
  host,
}) => {
  const { files, bar, field, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  // Typing replaces; Enter writes it and moves down. What depends on the
  // cell follows, on the other tab too.
  await cell("Tasks", "D2").click();
  await app.keyboard.type("2000");
  await app.keyboard.press("Enter");
  await expect(cell("Tasks", "D2")).toHaveText("2,000.00");
  await expect(cell("Tasks", "E2")).toHaveText("4,000.00");
  await expect(cell("Tasks", "D5")).toHaveText("10,700.50");
  await expect(bar.locator("output")).toHaveText("Tasks!D3");
  // Only that cell is another in the file.
  const typed = with_(PLAN, { "<td>1200<td>": "<td>2000<td>" });
  await expect.poll(saved).toBe(typed);
  await expect(files.locator(".page-tools .notice")).toHaveText("Saved");

  // The formula bar: Escape puts back what the cell holds, Enter writes.
  await field.fill("nothing");
  await field.press("Escape");
  await expect(field).toHaveValue("8400.5");
  await field.fill("=D2/2");
  await field.press("Enter");
  await expect(cell("Tasks", "D3")).toHaveText("1,000.00");
  const halved = with_(typed, { "<td>8400.5<td>": "<td>=D2/2<td>" });
  await expect.poll(saved).toBe(halved);

  // F2 edits what is there; Escape leaves it; Tab writes and moves right.
  await cell("Tasks", "A2").click();
  await app.keyboard.press("F2");
  await app.keyboard.type("!");
  await app.keyboard.press("Escape");
  await expect(cell("Tasks", "A2")).toHaveText("Write");
  await app.keyboard.press("Enter");
  await app.keyboard.type(" it");
  await app.keyboard.press("Tab");
  await expect(cell("Tasks", "A2")).toHaveText("Write it");
  await expect(bar.locator("output")).toHaveText("Tasks!B2");
  const named = with_(halved, { "<td>Write<td>": "<td>Write it<td>" });
  await expect.poll(saved).toBe(named);

  // Undo and redo, by button and by key, each saved.
  const undo = files.getByRole("button", { name: "Undo" });
  const redo = files.getByRole("button", { name: "Redo" });
  await expect(redo).toBeDisabled();
  await undo.click();
  await expect(cell("Tasks", "A2")).toHaveText("Write");
  await expect.poll(saved).toBe(halved);
  await cell("Tasks", "A3").click();
  await app.keyboard.press("Control+z");
  await app.keyboard.press("Control+z");
  await expect(cell("Tasks", "D2")).toHaveText("1,200.00");
  await expect.poll(saved).toBe(PLAN);
  await expect(undo).toBeDisabled();
  await app.keyboard.press("Control+Shift+z");
  await app.keyboard.press("Control+y");
  await expect.poll(saved).toBe(halved);
  await redo.click();
  await expect.poll(saved).toBe(named);
  await expect(redo).toBeDisabled();
});

test("rows are put in and taken out, filled down and sorted: formulas follow on every tab, and the file with them", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  const menu = (name) => stage.getByRole("menuitem", { name });
  // A row's number opens its menu. The row put in pushes the formulas under
  // it, and the other tab's, one row on.
  await stage
    .locator('table[data-sheet="Tasks"] [data-row="3"].sheet-edge')
    .click();
  await menu("Insert row above").click();
  const wider = with_(PLAN, {
    "<tr><td>Book<td>To do<td>FALSE<td>8400.5<td>=D3*2":
      "<tr><td><td><td><td><td>\n<tr><td>Book<td>To do<td>FALSE<td>8400.5<td>=D4*2",
    "<td>300<td>=D4*2": "<td>300<td>=D5*2",
    "=COUNTIF(C2:C4,TRUE)<td>=SUM(D2:D4)<td>=SUM(E2:E4)":
      "=COUNTIF(C2:C5,TRUE)<td>=SUM(D2:D5)<td>=SUM(E2:E5)",
    "=Tasks!D5": "=Tasks!D6",
  });
  await expect.poll(saved).toBe(wider);
  await expect(cell("Tasks", "D6")).toHaveText("9,900.50");

  // Filled down from the row above: what was typed is copied, a formula
  // moves along.
  await cell("Tasks", "A2").click();
  await cell("Tasks", "E3").click({ modifiers: ["Shift"] });
  await app.keyboard.press("Control+d");
  await expect(cell("Tasks", "E3")).toHaveText("2,400.00");
  await expect.poll(saved).toBe(
    with_(wider, {
      "<tr><td><td><td><td><td>":
        "<tr><td>Write<td>Done<td>TRUE<td>1200<td>=D3*2",
    }),
  );

  // The menu key on a cell: its row is taken out, and all is as it was.
  await cell("Tasks", "B3").click();
  await app.keyboard.press("Shift+F10");
  await app.keyboard.press("ArrowDown");
  await expect(menu("Insert row below")).toBeFocused();
  await menu("Delete row").click();
  await expect.poll(saved).toBe(PLAN);

  // A header cell sorts its column. The rows under the header move, each
  // formula with its row; the total stays and still sums them.
  await cell("Tasks", "D1").getByRole("button").click();
  await menu("Sort A to Z").click();
  await expect(
    stage.locator('tr:nth-child(-n+5) > [data-column="1"]:not(.sheet-edge)'),
  ).toHaveText(["Task", "Brief", "Write", "Book", "Total"]);
  await expect(cell("Tasks", "E4")).toHaveText("16,801.00");
  await expect(cell("Tasks", "D5")).toHaveText("9,900.50");
  const sorted = PLAN.replace(
    /<tr><td>Write[\s\S]*=D4\*2/,
    [
      "<tr><td>Brief<td>In progress<td>FALSE<td>300<td>=D2*2",
      "<tr><td>Write<td>Done<td>TRUE<td>1200<td>=D3*2",
      "<tr><td>Book<td>To do<td>FALSE<td>8400.5<td>=D4*2",
    ].join("\n"),
  );
  await expect.poll(saved).toBe(sorted);
  // By the keys, the other way.
  await app.keyboard.press("Alt+ArrowDown");
  await menu("Sort Z to A").click();
  await expect(cell("Tasks", "A2")).toHaveText("Book");
  await expect(cell("Tasks", "E2")).toHaveText("16,801.00");

  // A formula over a range that the sort would split has no place after
  // it: nothing is sorted, and the pane says why.
  await cell("Tasks", "E2").click();
  await app.keyboard.type("=SUM(D2:D3)");
  await app.keyboard.press("Enter");
  await expect.poll(saved).toContain("<td>8400.5<td>=SUM(D2:D3)");
  const written = await saved();
  await cell("Tasks", "D1").getByRole("button").click();
  await menu("Sort A to Z").click();
  await expect(files.locator(".page-tools .notice")).toHaveText(
    "Not sorted: =SUM(D2:D3) cannot be moved with its cell.",
  );
  await expect(cell("Tasks", "A2")).toHaveText("Book");
  expect(await saved()).toBe(written);
});

test("the keys move and stretch the selection, Delete clears it, and a column is put in from its letter", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, bar, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  const at = bar.locator("output");
  await cell("Tasks", "A2").click();
  // With Ctrl an arrow runs to the end of what is filled, then to the next
  // filled cell; Home and End go to the row's ends, with Ctrl the tab's.
  for (const [key, ref] of [
    ["Control+ArrowDown", "A5"],
    ["Control+ArrowRight", "C5"],
    ["End", "E5"],
    ["Home", "A5"],
    ["Control+Home", "A1"],
    ["Tab", "B1"],
    ["Shift+Tab", "A1"],
    ["ArrowDown", "A2"],
    ["ArrowDown", "A3"],
  ]) {
    await app.keyboard.press(key);
    await expect(at).toHaveText(`Tasks!${ref}`);
  }
  await app.keyboard.press("Shift+ArrowRight");
  await expect(stage.locator(".sheet-in")).toHaveText(["Book", "To do"]);
  await app.keyboard.press("Delete");
  await expect(cell("Tasks", "A3")).toHaveText("");
  const cleared = with_(PLAN, { "<td>Book<td>To do<td>": "<td><td><td>" });
  await expect.poll(saved).toBe(cleared);

  // A column's letter opens its menu. The column put in is a header in the
  // header row, and every formula reads one column on.
  await stage.locator('.sheet-letters [data-column="2"]').click();
  await stage.getByRole("menuitem", { name: "Insert column left" }).click();
  await expect(cell("Tasks", "F2")).toHaveText("2,400.00");
  await expect.poll(saved).toBe(
    sheet("Plan", {
      Tasks: [
        '<col><col><col data-options="To do|In progress|Done"><col data-control="checkbox"><col data-format="#,##0.00"><col>',
        "<th>Task<th><th>Status<th>Done<th>Cost<th>Double",
        "<td>Write<td><td>Done<td>TRUE<td>1200<td>=E2*2",
        "<td><td><td><td>FALSE<td>8400.5<td>=E3*2",
        "<td>Brief<td><td>In progress<td>FALSE<td>300<td>=E4*2",
        '<tr class="total"><td>Total<td><td><td>=COUNTIF(D2:D4,TRUE)<td>=SUM(E2:E4)<td>=SUM(F2:F4)',
      ],
      Inputs: ["<th>Name<th>Value", "<td>Budget<td>=Tasks!E5"],
    }),
  );
});

test("a filter hides the rows that do not match and writes nothing", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, bar, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  const writes = [];
  app.on("request", (request) => {
    if (
      request.method() === "POST" &&
      /\/files$/.test(new URL(request.url()).pathname)
    )
      writes.push(request.url());
  });
  await cell("Tasks", "B1").getByRole("button").click();
  const words = stage.getByRole("searchbox", { name: "Filter: contains" });
  await words.fill("DO");
  await words.press("Enter");
  // "Done" and "To do" stay; the total is never filtered out.
  await expect(cell("Tasks", "A4")).toBeHidden();
  await expect(cell("Tasks", "A3")).toBeVisible();
  await expect(cell("Tasks", "A5")).toBeVisible();
  await expect(files.locator(".sheet-note")).toHaveText("1 row hidden");
  // The arrows pass over what is hidden.
  await cell("Tasks", "A3").click();
  await app.keyboard.press("ArrowDown");
  await expect(bar.locator("output")).toHaveText("Tasks!A5");
  // Cleared, every row is back.
  const filtered = cell("Tasks", "B1").getByRole("button", { pressed: true });
  await filtered.click();
  await expect(words).toHaveValue("do");
  await stage.getByRole("menuitem", { name: "Clear filter" }).click();
  await expect(cell("Tasks", "A4")).toBeVisible();
  await expect(filtered).toHaveCount(0);
  await expect(files.locator(".sheet-note")).toHaveCount(0);
  expect(writes).toEqual([]);
  expect(await saved()).toBe(PLAN);
});

test("what a filter hides is neither cleared nor deleted unseen, and a column put in ends the filter", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  await cell("Tasks", "B1").getByRole("button").click();
  const words = stage.getByRole("searchbox", { name: "Filter: contains" });
  await words.fill("do");
  await words.press("Enter");
  await expect(cell("Tasks", "A4")).toBeHidden();
  // A box over the hidden row: Delete clears only what is seen.
  await cell("Tasks", "A3").click();
  await cell("Tasks", "A5").click({ modifiers: ["Shift"] });
  await app.keyboard.press("Delete");
  const cleared = with_(PLAN, {
    "<td>Book<td>": "<td><td>",
    "<td>Total<td>": "<td><td>",
  });
  await expect.poll(saved).toBe(cleared);
  // Its rows are not taken out while one of them is hidden.
  await app.keyboard.press("Shift+F10");
  await stage.getByRole("menuitem", { name: "Delete 3 rows" }).click();
  await expect(files.locator(".page-tools .notice")).toHaveText(
    "Clear the filter to delete these rows.",
  );
  expect(await saved()).toBe(cleared);
  // The filter was of column B: with a column before it, it is off.
  await stage.locator('.sheet-letters [data-column="1"]').click();
  await stage.getByRole("menuitem", { name: "Insert column left" }).click();
  await expect(cell("Tasks", "B4")).toHaveText("Brief");
  await expect(files.locator(".sheet-note")).toHaveCount(0);
});

// What the clipboard does in the grid: the event a copy, a cut or a paste
// raises, with one store of data the test keeps between them.
const clipboard = async (stage) => {
  const editor = stage.locator("input.sheet-editor");
  await editor.evaluate(() => {
    window.held = new DataTransfer();
  });
  return (kind, text) =>
    editor.evaluate(
      (field, [kind, text]) => {
        if (text !== undefined) window.held.setData("text/plain", text);
        field.dispatchEvent(
          new ClipboardEvent(kind, {
            clipboardData: window.held,
            bubbles: true,
            cancelable: true,
          }),
        );
        return window.held.getData("text/plain");
      },
      [kind, text],
    );
};

test("a block is pasted, cells are copied with their formulas, a box is ticked and a status chosen from its list", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  const act = await clipboard(stage);
  // Two rows of two cells from another spreadsheet land from the cell on.
  await cell("Tasks", "A3").click();
  await act("paste", "Rent\tDone\r\nPower\tTo do\r\n");
  await expect(cell("Tasks", "B4")).toHaveText("To do");
  const pasted = with_(PLAN, {
    "<td>Book<td>To do<td>": "<td>Rent<td>Done<td>",
    "<td>Brief<td>In progress<td>": "<td>Power<td>To do<td>",
  });
  await expect.poll(saved).toBe(pasted);

  // Copied here, cells go out as what they show and come back as what was
  // typed: a formula reads from where it lands.
  await cell("Tasks", "D2").click();
  await cell("Tasks", "E2").click({ modifiers: ["Shift"] });
  expect(await act("copy")).toBe("1,200.00\t2,400.00");
  await cell("Tasks", "D4").click();
  await act("paste");
  await expect(cell("Tasks", "E4")).toHaveText("2,400.00");
  const copied = with_(pasted, { "<td>300<td>=D4*2": "<td>1200<td>=D4*2" });
  await expect.poll(saved).toBe(copied);
  // Cut, they move as they are and leave their cells empty.
  await cell("Tasks", "E2").click();
  await act("cut");
  await cell("Tasks", "A5").click();
  await act("paste");
  await expect(cell("Tasks", "E2")).toHaveText("");
  const cut = with_(copied, {
    "<td>1200<td>=D2*2": "<td>1200<td>",
    "<td>Total<td>": "<td>=D2*2<td>",
  });
  await expect.poll(saved).toBe(cut);

  // After a change the cut is forgotten: what was shown is pasted as text,
  // and no cell is cleared where the cut cells lay.
  await cell("Tasks", "A2").click();
  await act("cut");
  await cell("Tasks", "A4").click();
  await app.keyboard.type("Fees");
  await app.keyboard.press("Enter");
  await cell("Tasks", "B5").click();
  await act("paste");
  await expect(cell("Tasks", "B5")).toHaveText("Write");
  await expect(cell("Tasks", "A2")).toHaveText("Write");
  // A paste of nothing changes nothing.
  await cell("Tasks", "A2").click();
  await act("paste", "");
  await expect(cell("Tasks", "A2")).toHaveText("Write");
  const forgotten = with_(cut, {
    "<td>Power<td>": "<td>Fees<td>",
    "<td>=D2*2<td><td>": "<td>=D2*2<td>Write<td>",
  });
  await expect.poll(saved).toBe(forgotten);

  // A box is ticked by a click or Space; the count of ticks follows.
  await cell("Tasks", "C3").getByRole("checkbox").click();
  await expect(cell("Tasks", "C5")).toHaveText("2");
  await cell("Tasks", "C4").click();
  await app.keyboard.press("Space");
  await expect(cell("Tasks", "C4").getByRole("checkbox")).toBeChecked();
  const ticked = forgotten.replaceAll("<td>FALSE<td>", "<td>TRUE<td>");
  await expect.poll(saved).toBe(ticked);

  // A status is chosen from its column's list.
  await cell("Tasks", "B2").dblclick();
  const list = stage.getByRole("combobox", { name: "Options" });
  await expect(list.locator("option")).toHaveText([
    "",
    "To do",
    "In progress",
    "Done",
  ]);
  await list.selectOption("In progress");
  await expect(cell("Tasks", "B2")).toHaveText("In progress");
  await expect(list).toHaveCount(0);
  await expect.poll(saved).toBe(
    with_(ticked, {
      "<td>Write<td>Done<td>": "<td>Write<td>In progress<td>",
    }),
  );
});

test("a save the file refuses, because it was rewritten meanwhile, shows the sheet as the file is and says so", async ({
  page: app,
  session,
  host,
}) => {
  const { files, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  const type = async (ref, text) => {
    await cell("Tasks", ref).click();
    await app.keyboard.type(text);
    await app.keyboard.press("Enter");
  };
  await type("A2", "Draft");
  await expect.poll(saved).toContain("<td>Draft<td>");
  // The agent's version, while the sheet is open as it was.
  const rewritten = with_(PLAN, { "<td>Book<td>": "<td>Reserve<td>" });
  await writeFiles(host.project, { "sheets/plan.html": rewritten });
  await type("A4", "Lost");
  const notice = files.locator(".page-notice");
  await expect(notice).toContainText(
    "The last change was not saved: the file changed meanwhile.",
  );
  await expect(cell("Tasks", "A3")).toHaveText("Reserve");
  await expect(cell("Tasks", "A2")).toHaveText("Write");
  await expect(cell("Tasks", "A4")).toHaveText("Brief");
  expect(await saved()).toBe(rewritten);
  await expect(files.getByRole("button", { name: "Undo" })).toBeDisabled();
  // From there on changes are saved again, and the notice goes.
  await type("A4", "Kept");
  await expect
    .poll(saved)
    .toBe(with_(rewritten, { "<td>Brief<td>": "<td>Kept<td>" }));
  await expect(notice).toHaveCount(0);
});

test("a cell written with a bare < is reported, and its tab is not edited over it", async ({
  page: app,
  session,
  host,
}) => {
  // The formula's "<D2…" reads as a tag. The tab also holds more than it has.
  const broken = with_(PLAN, {
    "<td>300<td>=D4*2": '<td>300<td>=IF(D4<D2,"low","high")',
    'data-sheet="Tasks"': 'data-sheet="Tasks" data-frozen="9 Z"',
  });
  const { files, tabs, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    broken,
  );
  const why = "Tasks!E4 holds markup: write < as &lt;";
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  await expect(app.locator("#prompt")).toHaveValue(new RegExp(why));
  await cell("Tasks", "A2").click();
  await app.keyboard.type("Draft");
  await app.keyboard.press("Enter");
  await expect(files.locator(".page-tools .notice")).toHaveText(why);
  await expect(cell("Tasks", "A2")).toHaveText("Write");
  // Another tab is edited as ever.
  await tabs.nth(1).click();
  await cell("Inputs", "A2").click();
  await app.keyboard.type("Sum");
  await app.keyboard.press("Enter");
  await expect
    .poll(saved)
    .toBe(with_(broken, { "<td>Budget<td>": "<td>Sum<td>" }));
});

test("the agent's write to another file leaves the open sheet, its undo and its saving as they are", async ({
  page: app,
  session,
  host,
  command,
}) => {
  const { files, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  const type = async (ref, text) => {
    await cell("Tasks", ref).click();
    await app.keyboard.type(text);
    await app.keyboard.press("Enter");
  };
  await type("A2", "Draft");
  const drafted = with_(PLAN, { "<td>Write<td>": "<td>Draft<td>" });
  await expect.poll(saved).toBe(drafted);
  await selectProbeModel(command, session, { yolo: true });
  await app.locator("#prompt").fill("Design probe");
  await app.locator("#prompt").press("Enter");
  await expect
    .poll(() =>
      readFile(join(host.project, "design/01-hero-a.html"), "utf8").catch(
        () => "",
      ),
    )
    .toContain("Rewritten");
  await type("A3", "Still");
  await expect
    .poll(saved)
    .toBe(with_(drafted, { "<td>Book<td>": "<td>Still<td>" }));
  // Both changes are still the sheet's to take back.
  await files.getByRole("button", { name: "Undo" }).click();
  await files.getByRole("button", { name: "Undo" }).click();
  await expect.poll(saved).toBe(PLAN);
});

test("a range's numbers are summed, and a comment on it goes into the draft", async ({
  page: app,
  session,
  host,
}) => {
  const { files, cell } = await openSheet(app, session, host.project, PLAN);
  // Dragged from one cell to the next, then stretched with Shift and an
  // arrow.
  await cell("Tasks", "D2").hover();
  await app.mouse.down();
  await cell("Tasks", "D3").hover();
  await app.mouse.up();
  await expect(files.locator(".sheet-note")).toHaveText(
    "Sum 9,600.5 · Average 4,800.25 · Count 2",
  );
  await app.keyboard.press("Shift+ArrowDown");
  await expect(files.locator(".sheet-note")).toHaveText(
    "Sum 9,900.5 · Average 3,300.17 · Count 3",
  );
  await files.getByRole("button", { name: "Comment" }).click();
  await expect(files.getByText("Comment on Tasks!D2:D4")).toBeVisible();
  await files.getByLabel("Comment", { exact: true }).fill("too high");
  await files.getByRole("button", { name: "Add to chat" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    "`sheets/plan.html` `Tasks!D2:D4` — too high",
  );
});

test("the grid runs on past the table: what is typed there joins it, and the file holds only what is used", async ({
  page: app,
  session,
  host,
}) => {
  const { files, stage, bar, tabs, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  const at = bar.locator("output");
  await tabs.nth(1).click();
  // The arrows leave the table of two columns and two rows.
  await cell("Inputs", "B2").click();
  for (let step = 0; step < 3; step++) await app.keyboard.press("ArrowRight");
  await expect(at).toHaveText("Inputs!E2");
  await app.keyboard.type("x");
  await app.keyboard.press("Enter");
  await cell("Inputs", "B2").click();
  for (let step = 0; step < 5; step++) await app.keyboard.press("ArrowDown");
  await expect(at).toHaveText("Inputs!B7");
  await app.keyboard.type("=B2*2");
  await app.keyboard.press("Enter");
  await expect(cell("Inputs", "B7")).toHaveText("19,801.00");
  // The table reaches to both and no further: a header row stays one.
  const reached = with_(PLAN, {
    "<tr><th>Name<th>Value\n<tr><td>Budget<td>=Tasks!D5":
      "<tr><th>Name<th>Value<th><th><th>\n<tr><td>Budget<td>=Tasks!D5<td><td><td>x" +
      "\n<tr><td><td><td><td><td>".repeat(4) +
      "\n<tr><td><td>=B2*2<td><td><td>",
  });
  await expect.poll(saved).toBe(reached);
  // Scrolled to its end the grid has more rows to show.
  const rows = () =>
    stage.locator('table[data-sheet="Inputs"] tbody tr').count();
  const before = await rows();
  expect(before).toBeGreaterThan(7);
  await stage
    .locator("html")
    .evaluate((root) => scrollTo(0, root.scrollHeight));
  await expect.poll(rows).toBeGreaterThan(before);
  // Cleared, what was reached is given back; Undo takes back the rest.
  const less = with_(PLAN, {
    "<tr><th>Name<th>Value\n<tr><td>Budget<td>=Tasks!D5":
      "<tr><th>Name<th>Value<th><th><th>\n<tr><td>Budget<td>=Tasks!D5<td><td><td>x",
  });
  await cell("Inputs", "B7").click();
  await app.keyboard.press("Delete");
  await expect.poll(saved).toBe(less);
  const undo = files.getByRole("button", { name: "Undo" });
  await undo.click();
  await expect.poll(saved).toBe(reached);
  await undo.click();
  await expect.poll(saved).toBe(less);
  await undo.click();
  await expect.poll(saved).toBe(PLAN);
  await files.getByRole("button", { name: "Redo" }).click();
  await files.getByRole("button", { name: "Redo" }).click();
  await expect.poll(saved).toBe(reached);
  // Read anew it is the same sheet, and nothing is written for that.
  await stage.locator("html").evaluate(() => location.reload());
  await expect(cell("Inputs", "E2")).toHaveText("x");
  await expect(cell("Inputs", "B7")).toHaveText("19,801.00");
  expect(await saved()).toBe(reached);
});

test("a sheet with nothing in it shows a pane of cells to type in", async ({
  page: app,
  session,
  host,
}) => {
  const blank = sheet("Blank", { One: [] });
  const { stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    blank,
  );
  // A cell lies under the pane's far corner.
  await expect(cell("One", "A1")).toBeVisible();
  expect(
    await stage
      .locator("html")
      .evaluate(
        () =>
          document
            .elementFromPoint(innerWidth - 24, innerHeight - 24)
            ?.closest("td")?.dataset.row,
      ),
  ).toBeTruthy();
  expect(await saved()).toBe(blank);
  await cell("One", "C3").click();
  await app.keyboard.type("here");
  await app.keyboard.press("Enter");
  await expect
    .poll(saved)
    .toBe(
      blank.replace(
        "\n</table>",
        "<tr><td><td><td>\n<tr><td><td><td>\n<tr><td><td><td>here\n</table>",
      ),
    );
});

test("on a phone the rows and columns a sheet holds stay in view, and a cell is edited from the formula bar", async ({
  page: app,
  session,
  host,
}) => {
  // Forty rows of twelve columns, the first row and column A held.
  const year = sheet("Year", {
    Months: Array.from({ length: 40 }, (_, row) =>
      Array.from({ length: 12 }, (_, column) =>
        row
          ? `<td>${column ? row * column : `Row ${row}`}`
          : `<th>Column ${column + 1}`,
      ).join(""),
    ),
  }).replace('data-sheet="Months"', 'data-sheet="Months" data-frozen="1 A"');
  const { files, stage, bar, field, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    year,
    { phone: true },
  );
  await expect(cell("Months", "B2")).toBeInViewport();
  await cell("Months", "L40").evaluate((last) => last.scrollIntoView());
  await expect(cell("Months", "B2")).not.toBeInViewport();
  for (const held of ["A1", "L1", "A40"])
    await expect(cell("Months", held)).toBeInViewport();
  // Each letter stays over its own column.
  const left = (cell) =>
    cell.evaluate((drawn) => Math.round(drawn.getBoundingClientRect().left));
  for (const column of [1, 12])
    expect(
      await left(stage.locator(`.sheet-letters [data-column="${column}"]`)),
    ).toBe(
      await left(stage.locator(`[data-row="40"][data-column="${column}"]`)),
    );
  // A tap selects; no key is needed to say where.
  await cell("Months", "L40").click();
  await expect(bar.locator("output")).toHaveText("Months!L40");
  await expect(field).toHaveValue("429");
  // Edit cell brings the formula bar's field up to type in.
  await files.getByRole("button", { name: "Edit cell" }).click();
  await expect(field).toBeFocused();
  await field.fill("=K40+1");
  await field.press("Enter");
  await expect(cell("Months", "L40")).toHaveText("391");
  await expect.poll(saved).toContain("<td>390<td>=K40+1\n</table>");
  await expect(files.getByRole("button", { name: "Comment" })).toBeVisible();
});

test("a rule strikes, fills and emboldens what matches, until an edit ends the match; one not understood is reported", async ({
  page: app,
  session,
  host,
}) => {
  const ruled = sheet("Plan", {
    Tasks: [
      '<col><col><col data-rule="< 0: bad; > 1000: bold">',
      "<th>Task<th>Status<th>Cost",
      "<td>Write<td>Done<td>1200",
      '<tr data-fill="warn"><td>Book<td>To do<td>-5',
    ],
  }).replace(
    'data-sheet="Tasks"',
    'data-sheet="Tasks" data-rule="B = Done: strike muted; B = Nope: sparkle"',
  );
  const { files, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    ruled,
  );
  // The table's rule is on the whole row, the column's on its own cell.
  for (const ref of ["A2", "C2"]) {
    await expect(cell("Tasks", ref)).toHaveClass(/sheet-strike/);
    await expect(cell("Tasks", ref)).toHaveAttribute("data-fill", "muted");
  }
  await expect(cell("Tasks", "C2")).toHaveClass(/sheet-bold/);
  // A rule's fill lies over the row's own.
  await expect(cell("Tasks", "A3")).toHaveAttribute("data-fill", "warn");
  await expect(cell("Tasks", "C3")).toHaveAttribute("data-fill", "bad");
  await expect(cell("Tasks", "A3")).not.toHaveClass(/sheet-strike/);
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    /Tasks: rule `B = Nope: sparkle` is not understood/,
  );

  await cell("Tasks", "B2").click();
  await app.keyboard.type("To do");
  await app.keyboard.press("Enter");
  await expect(cell("Tasks", "A2")).not.toHaveClass(/sheet-strike/);
  await expect(cell("Tasks", "A2")).not.toHaveAttribute("data-fill");
  await expect(cell("Tasks", "C2")).toHaveClass(/sheet-bold/);
  // The rules are in the file as they were written.
  await expect
    .poll(saved)
    .toBe(with_(ruled, { "<td>Write<td>Done<td>": "<td>Write<td>To do<td>" }));
});

// Presses on the middle of `from` and lets go `x` and `y` away.
const drag = async (app, from, x, y) => {
  await from.hover();
  await app.mouse.down();
  const box = await from.boundingBox();
  await app.mouse.move(box.x + box.width / 2 + x, box.y + box.height / 2 + y, {
    steps: 4,
  });
  await app.mouse.up();
};

test("a chart is drawn from its cells, follows an edit, is put in from a selection, and is saved with its tab", async ({
  page: app,
  session,
  host,
}) => {
  const figure = (type, range, at, caption = "") =>
    `<figure data-chart="${type}" data-range="${range}" data-at="${at}" data-size="6x12"><figcaption>${caption}</figcaption></figure>`;
  const costs = figure("bar", "A1:C4", "E2", "Costs per item");
  const charted = sheet("Plan", {
    Costs: [
      "<th>Item<th>Net<th>Tax",
      "<td>Rent<td>1200<td>240",
      "<td>Power<td>90<td>18",
      "<td>Water<td>60<td>12",
      costs,
    ],
  });
  const { files, stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    charted,
  );
  const chart = stage.getByRole("img", { name: "Costs per item" });
  const heights = () =>
    chart
      .locator("rect")
      .evaluateAll((bars) => bars.map((bar) => bar.height.baseVal.value));
  // A bar for each number, the series named beside them.
  await expect(chart.locator("rect")).toHaveCount(6);
  await expect(stage.locator(".sheet-chart li")).toHaveText(["Net", "Tax"]);
  expect(Math.max(...(await heights()))).toBe((await heights())[0]);
  await cell("Costs", "B3").click();
  await app.keyboard.type("2400");
  await app.keyboard.press("Enter");
  await expect
    .poll(async () => (await heights()).indexOf(Math.max(...(await heights()))))
    .toBe(1);

  // Its own menu changes its kind.
  await stage.getByRole("button", { name: /^Chart menu/ }).click();
  await stage.getByRole("menuitem", { name: "Pie" }).click();
  await expect(chart.locator("path")).toHaveCount(3);
  const pie = with_(charted, {
    "<td>90<td>": "<td>2400<td>",
    [costs]: figure("pie", "A1:C4", "E2", "Costs per item"),
  });
  await expect.poll(saved).toBe(pie);

  // A chart of the selection, put in beside it.
  await cell("Costs", "A1").click();
  await cell("Costs", "B4").click({ modifiers: ["Shift"] });
  await files.getByRole("button", { name: "Chart", exact: true }).click();
  await expect(
    stage.getByRole("img", { name: "Bars of A1:B4" }).locator("rect"),
  ).toHaveCount(3);
  const two = pie.replace(
    "</figure>",
    `</figure>\n${figure("bar", "A1:B4", "D1")}`,
  );
  await expect.poll(saved).toBe(two);
  // Undo takes it out again.
  await files.getByRole("button", { name: "Undo" }).click();
  await expect.poll(saved).toBe(pie);

  // Dragged, the chart sits on the cell under its corner; by its own corner
  // it covers more, in cells of 96 by 24 wherever it lies.
  await drag(app, stage.locator(".sheet-chart figcaption"), 200, 5);
  await expect.poll(saved).toContain('data-at="G2" data-size="6x12"');
  await drag(app, stage.locator(".sheet-size"), 110, 30);
  await expect.poll(saved).toContain('data-at="G2" data-size="7x13"');

  // The file's chart is there when the sheet is read anew; deleted, it is
  // gone from the file.
  await stage.locator("html").evaluate(() => location.reload());
  await expect(chart.locator("path")).toHaveCount(3);
  await stage.getByRole("button", { name: /^Chart menu/ }).click();
  await stage.getByRole("menuitem", { name: "Delete chart" }).click();
  await expect(stage.locator(".sheet-chart")).toHaveCount(0);
  await expect.poll(saved).toBe(
    with_(pie, {
      [`\n${figure("pie", "A1:C4", "E2", "Costs per item")}`]: "",
    }),
  );
});

test("a chart moved from narrow columns to wide ones keeps its size, and only its place changes in the file", async ({
  page: app,
  session,
  host,
}) => {
  const chart = (at) =>
    `<figure data-chart="bar" data-range="A1:B3" data-at="${at}" data-size="3x8"><figcaption>Costs</figcaption></figure>`;
  const plan = (at) =>
    sheet("Plan", {
      Costs: [
        '<col><col><col><col data-width="320"><col data-width="320">',
        "<th>Item<th>Net<th><th><th>",
        "<td>Rent<td>1200<td><td><td>",
        "<td>Power<td>90<td><td><td>",
        chart(at),
      ],
    });
  const { stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    plan("A5"),
  );
  const figure = stage.locator(".sheet-chart");
  const size = async () => {
    const { width, height } = await figure.boundingBox();
    return [width, height];
  };
  const before = await size();
  expect(before).toEqual([3 * 96, 8 * 24]);
  const from = await figure.boundingBox();
  const to = await cell("Costs", "D1").boundingBox();
  await drag(app, figure.locator("figcaption"), to.x - from.x + 4, 0);
  await expect.poll(saved).toBe(plan("D5"));
  expect(await size()).toEqual(before);
});

test("a figure that does not directly follow its table is reported and left in the file; one left open loses no tab", async ({
  page: app,
  session,
  host,
}) => {
  const open =
    '<figure data-chart="bar" data-range="A1:B3"><figcaption>Costs</figcaption>';
  // After the first table a comment, then a chart; then one with no end
  // tag, which takes the second table inside it.
  const apart = sheet("Plan", {
    Costs: ["<th>Item<th>Net", "<td>Rent<td>1200", "<td>Power<td>90"],
    Notes: ["<td>Keep me"],
  }).replace(
    "</table>\n<table",
    `</table>\n<!-- chart -->\n${open}</figure>\n${open}\n<table`,
  );
  const { files, stage, tabs, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    apart,
  );
  await expect(tabs).toHaveText(["Costs", "Notes"]);
  await expect(cell("Costs", "B3")).toHaveText("90");
  await expect(stage.locator(".sheet-chart")).toHaveCount(0);
  await files.getByRole("button", { name: "Ask agent to fix" }).click();
  await expect(app.locator("#prompt")).toHaveValue(
    /chart A1:B3: a chart must directly follow its table/,
  );
  // Each tab is saved in its place; no figure is taken, added or lost.
  await cell("Costs", "B2").click();
  await app.keyboard.type("1500");
  await app.keyboard.press("Enter");
  const first = with_(apart, { "<td>Rent<td>1200": "<td>Rent<td>1500" });
  await expect.poll(saved).toBe(first);
  await tabs.nth(1).click();
  await cell("Notes", "A1").click();
  await app.keyboard.type("Kept");
  await app.keyboard.press("Enter");
  await expect.poll(saved).toBe(with_(first, { "<td>Keep me": "<td>Kept" }));
});
