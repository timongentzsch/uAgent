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
  openAlone,
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
// Scrolls the grid until `row` is the first under what is held: where the
// grid ends before it, a pane more comes with each frame at its end.
const toRow = (stage, row) =>
  stage.locator("html").evaluate(async (_, row) => {
    const high = document.querySelector(
      "table:not([hidden]) tbody tr:last-child",
    ).offsetHeight;
    for (let frames = 0; frames < 2000; frames++) {
      scrollTo(scrollX, (row - 1) * high);
      if (scrollY >= (row - 1) * high - 1) break;
      await new Promise(requestAnimationFrame);
    }
  }, row);
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
  const { files, stage, bar, field, cell, saved } = await openSheet(
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

  // The formula bar shows what is being typed in the cell, and what the
  // cell holds once that is given up.
  await app.evaluate(() => {
    window.said = [];
    addEventListener("message", ({ data }) => {
      if (data?.uagentDesign === 1 && data.type === "typing")
        window.said.push(Object.keys(data).sort().join(" "));
    });
  });
  const said = () => app.evaluate(() => window.said.splice(0));
  await app.keyboard.type("77");
  await expect(field).toHaveValue("77");
  await app.keyboard.press("Escape");
  await expect(field).toHaveValue("8400.5");
  await expect.poll(saved).toBe(typed);
  // The page says the table once, cut around the text; a key then says
  // only its text, and giving up says it is over.
  const key = "text type uagentDesign written";
  expect(await said()).toEqual([
    `head tab tabs tail ${key}`,
    key,
    "type uagentDesign",
  ]);
  // Text that is the cell's own again is no edit to keep.
  await app.keyboard.press("F2");
  await app.keyboard.type("1");
  await expect(field).toHaveValue("8400.51");
  await app.keyboard.press("Backspace");
  await expect(field).toHaveValue("8400.5");
  expect((await said()).at(-1)).toBe("type uagentDesign");
  await app.keyboard.press("Escape");

  // The formula bar: what is typed there is typed in the cell. Escape puts
  // back what the cell holds, Enter writes.
  const editor = stage.locator("input.sheet-editor");
  await field.fill("nothing");
  await expect(editor).toHaveValue("nothing");
  await field.press("Escape");
  await expect(field).toHaveValue("8400.5");
  await expect(editor).not.toHaveAttribute("data-editing");
  await field.fill("=D2/2");
  await field.press("Enter");
  await expect(cell("Tasks", "D3")).toHaveText("1,000.00");
  const halved = with_(typed, { "<td>8400.5<td>": "<td>=D2/2<td>" });
  await expect.poll(saved).toBe(halved);
  // Left by a press on another cell, the field writes into its own cell;
  // left for a control, it writes before the control acts.
  await field.fill("=D2/4");
  await cell("Tasks", "B4").click();
  await expect(cell("Tasks", "D3")).toHaveText("500.00");
  await expect(cell("Tasks", "B4")).toHaveText("In progress");
  await expect(bar.locator("output")).toHaveText("Tasks!B4");
  await cell("Tasks", "D3").click();
  await expect(field).toHaveValue("=D2/4");
  await field.fill("=D2/2");
  await field.press("Tab");
  await expect(cell("Tasks", "D3")).toHaveText("1,000.00");
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
  for (let step = 0; step < 4; step++) await app.keyboard.press("Control+z");
  await expect(cell("Tasks", "D2")).toHaveText("1,200.00");
  await expect.poll(saved).toBe(PLAN);
  await expect(undo).toBeDisabled();
  for (const key of ["Shift+z", "y", "Shift+z", "y"])
    await app.keyboard.press(`Control+${key}`);
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
  await expect(menu("Cut")).toBeFocused();
  await app.keyboard.press("ArrowDown");
  await expect(menu("Copy")).toBeFocused();
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
  // Inside the table Ctrl and an arrow stop at its last row, stretching the
  // selection too; from there, and from outside it, they run on to the
  // spreadsheet's last row and column, and back to what is filled.
  await cell("Tasks", "A2").click();
  await app.keyboard.press("Control+Shift+ArrowDown");
  await expect(stage.locator(".sheet-in")).toHaveCount(4);
  await cell("Tasks", "A5").click();
  for (const [key, ref] of [
    ["Control+ArrowDown", "A1048576"],
    ["Control+ArrowRight", "XFD1048576"],
    ["Control+ArrowLeft", "A1048576"],
    ["Control+ArrowUp", "A5"],
  ]) {
    await app.keyboard.press(key);
    await expect(at).toHaveText(`Tasks!${ref}`);
    await expect(cell("Tasks", ref)).toBeInViewport();
  }
  await cell("Tasks", "A3").click();
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
  // What is being typed stays while its cell is scrolled out of what is
  // drawn and back.
  await cell("Inputs", "A9").click();
  await app.keyboard.type("kept");
  await toRow(stage, 4990);
  await expect(cell("Inputs", "A5000")).toBeInViewport();
  await expect(cell("Inputs", "A9")).toHaveCount(0);
  await toRow(stage, 1);
  await expect(cell("Inputs", "A9")).toBeInViewport();
  await expect(stage.locator("input.sheet-editor")).toHaveValue("kept");
  await app.keyboard.press("Escape");
  // Typed five thousand rows down, the file holds every row between, and
  // Undo gives them back.
  await toRow(stage, 4990);
  await cell("Inputs", "A5000").click();
  await app.keyboard.type("far");
  await app.keyboard.press("Enter");
  await expect
    .poll(async () => (await saved()).match(/\n<tr/g).length)
    .toBe(5 + 5000);
  expect((await saved()).length).toBeLessThan(200_000);
  await files.getByRole("button", { name: "Undo" }).click();
  await expect.poll(saved).toBe(reached);
  // At the spreadsheet's last row it is refused: the file would be too large.
  await cell("Inputs", "A4995").click();
  await app.keyboard.press("Control+ArrowDown");
  await expect(at).toHaveText("Inputs!A1048576");
  await app.keyboard.type("too far");
  await app.keyboard.press("Enter");
  await expect(files.locator(".page-tools .notice")).toHaveText(
    "A1048576 is too far from the table to keep: the file would hold every row between.",
  );
  expect(await saved()).toBe(reached);
  // The scrollbar runs over a stretch of the grid there: a key leads home.
  await app.keyboard.press("Control+Home");
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

// The page is left by the press that would end the typing: what was typed
// reaches the app as it is typed, and leaving writes it. Time is stopped,
// so nothing else can have.
for (const touch of [false, true])
  for (const [leaving, name, where] of [
    ["Back", /^Back to/, "a cell"],
    ["Close files", "Close files", "a cell"],
    ["Back", /^Back to/, "the formula bar"],
    ["Close files", "Close files", "the formula bar"],
  ])
    test(`what is typed in ${where} is in its file when a ${touch ? "tap" : "click"} on ${leaving} leaves the sheet at once`, async ({
      browser,
      storageState,
      session,
      host,
    }) => {
      const context = await browser.newContext({
        hasTouch: touch,
        storageState,
      });
      try {
        const app = await context.newPage();
        const { files, field, cell, saved } = await openSheet(
          app,
          session,
          host.project,
          PLAN,
        );
        await cell("Tasks", "A2").click();
        await app.clock.install();
        await app.clock.pauseAt(
          (await app.evaluate(() => Date.now())) + 60_000,
        );
        // The app has it once the page has said it: in the bar, shown.
        await app.evaluate(() =>
          addEventListener("message", ({ data }) => {
            if (data?.type === "typing") window.said = data.text;
          }),
        );
        if (where === "a cell") await app.keyboard.type("Draft");
        else await field.fill("Draft");
        await expect.poll(() => app.evaluate(() => window.said)).toBe("Draft");
        await expect(field).toHaveValue("Draft");
        const leave = files.getByRole("button", { name });
        await (touch ? leave.tap() : leave.click());
        await app.clock.resume();
        await expect
          .poll(saved)
          .toBe(with_(PLAN, { "<td>Write": "<td>Draft" }));
      } finally {
        await context.close();
      }
    });

test("past the table: a row or column is selected across the table only, a spare header sorts and filters nothing, and a filter stays when the table widens", async ({
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
  const picked = stage.locator(".sheet-in");
  const edge = (row) =>
    stage.locator(`table[data-sheet="Tasks"] [data-row="${row}"].sheet-edge`);
  // Five columns and five rows, whatever the grid shows beyond them.
  await edge(2).click();
  await expect(picked).toHaveCount(5);
  await app.keyboard.press("Escape");
  await edge(9).click();
  await expect(picked).toHaveCount(5);
  await app.keyboard.press("Escape");
  await stage.locator('.sheet-letters [data-column="2"]').click();
  await expect(picked).toHaveCount(5);
  await app.keyboard.press("Escape");
  // The header row's spare cell is no header of a column.
  await cell("Tasks", "G1").click({ button: "right" });
  await expect(
    stage.getByRole("menuitem", { name: "Insert chart" }),
  ).toBeVisible();
  await expect(stage.getByRole("menuitem", { name: /^Sort/ })).toHaveCount(0);
  await expect(
    stage.getByRole("searchbox", { name: "Filter: contains" }),
  ).toHaveCount(0);
  await app.keyboard.press("Escape");
  // A filter is kept when a spare column is typed in, and when it is cut.
  await cell("Tasks", "B1").getByRole("button").click();
  const words = stage.getByRole("searchbox", { name: "Filter: contains" });
  await words.fill("do");
  await words.press("Enter");
  await expect(files.locator(".sheet-note")).toHaveText("1 row hidden");
  await cell("Tasks", "G2").click();
  await app.keyboard.type("x");
  await app.keyboard.press("Enter");
  await expect.poll(saved).toContain("<td>=D2*2<td><td>x");
  await expect(files.locator(".sheet-note")).toHaveText("1 row hidden");
  await cell("Tasks", "G2").click();
  await app.keyboard.press("Delete");
  await expect.poll(saved).toBe(PLAN);
  await expect(files.locator(".sheet-note")).toHaveText("1 row hidden");
  await expect(cell("Tasks", "A4")).toBeHidden();
});

test("a chart placed far past the table is drawn there, and a chart's range may reach past the table", async ({
  page: app,
  session,
  host,
}) => {
  const far = sheet("Plan", {
    Costs: [
      "<th>Item<th>Net",
      "<td>Rent<td>1200",
      "<td>Power<td>90",
      '<figure data-chart="bar" data-range="A1:B9" data-at="A20000" data-size="3x8"><figcaption>Costs</figcaption></figure>',
    ],
  });
  const { stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    far,
  );
  const chart = stage.getByRole("img", { name: "Costs" });
  await expect(chart.locator("rect")).toHaveCount(2);
  await expect(chart).not.toBeInViewport();
  await toRow(stage, 19995);
  await expect(chart).toBeInViewport();
  // Its corner is on the cell it names.
  const [corner, named] = await Promise.all([
    stage.locator(".sheet-chart").boundingBox(),
    cell("Costs", "A20000").boundingBox(),
  ]);
  expect([corner.x, corner.y]).toEqual([named.x, named.y]);
  expect(await saved()).toBe(far);
});

test("a table of five thousand rows is drawn a screenful at a time, wherever it is scrolled to", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    sheet("Big", {
      Data: Array.from({ length: 5000 }, (_, row) =>
        row ? `<td>Row ${row + 1}<td>${row}` : "<th>Name<th>Value",
      ),
    }),
  );
  const rows = () => stage.locator("tbody tr").count();
  await expect(cell("Data", "A2")).toHaveText("Row 2");
  const screenful = await rows();
  expect(screenful).toBeLessThan(120);
  await toRow(stage, 2500);
  await expect(cell("Data", "A2510")).toHaveText("Row 2510");
  expect(await rows()).toBeLessThan(120);
  // The header row is held; a value typed there is saved as any.
  await expect(cell("Data", "A1")).toBeInViewport();
  await cell("Data", "B2510").click();
  await app.keyboard.type("7");
  await app.keyboard.press("Enter");
  await expect.poll(saved).toContain("<tr><td>Row 2510<td>7\n");
  expect(await rows()).toBeLessThan(120);
});

test("the editor and a chart lie on their cells: under wrapped rows of long words, on a held cell scrolled, at the grid's end; numbers are not cut short", async ({
  page: app,
  session,
  host,
}) => {
  const words = [
    "A long-running migration of the customer-records database, with rollback",
    "Reconciliation: accounts-receivable/accounts-payable, internationalization",
    "https://example.com/a/very/long/path/that/does/not/break/anywhere/index.html",
  ];
  const wrapped = sheet("Wrapped", {
    Work: [
      '<col><col class="wrap" data-width="180"><col data-format="#,##0.00"><col>',
      "<th>Task<th>Detail<th>Net<th>Id",
      ...Array.from(
        { length: 40 },
        (_, row) =>
          `<td>Task ${row + 1}<td>${row < 10 ? words[row % 3] : "Short"}<td>${row % 2 ? "1111111.11" : "711171.17"}<td>${"7".repeat(8)}`,
      ),
      '<figure data-chart="bar" data-range="A1:C4" data-at="F14" data-size="4x8"><figcaption>Net</figcaption></figure>',
    ],
  }).replace('data-sheet="Work"', 'data-sheet="Work" data-frozen="1 A"');
  const { files, stage, bar, cell } = await openSheet(
    app,
    session,
    host.project,
    wrapped,
  );
  const page = stage.locator("html");
  // The editor's box and the active cell's; a chart's corner and its cell's.
  const boxes = (...of) =>
    page.evaluate((_, of) => {
      const shown = "table:not([hidden]) ";
      return of.map((selector) => {
        const box = document
          .querySelector(selector.replace("^", shown))
          .getBoundingClientRect();
        return [box.left, box.top, box.width, box.height].map(Math.round);
      });
    }, of);
  // Typed in, the editor may be wider than its cell.
  const exact = async (sides = 4) => {
    await expect
      .poll(async () => {
        const [editor, active] = await boxes(
          "input.sheet-editor",
          '^[aria-selected="true"]',
        );
        return (
          `${editor.slice(0, sides)}` === `${active.slice(0, sides)}` || {
            editor,
            active,
          }
        );
      })
      .toBe(true);
  };
  // The letters' row is as high as a row of the app's type.
  await expect(cell("Work", "A1")).toBeVisible();
  const [letters, row] = await boxes("^thead tr", "^tbody tr:last-child");
  expect(letters[3]).toBe(row[3]);
  expect(row[3]).toBeGreaterThan(24);
  await cell("Work", "C13").click();
  await exact();
  const [chart, under] = await boxes(
    ".sheet-chart",
    '^[data-row="14"][data-column="6"]',
  );
  expect(chart.slice(0, 2)).toEqual(under.slice(0, 2));
  expect(
    await page.evaluate(() =>
      [...document.querySelectorAll("table:not([hidden]) td.sheet-number")]
        .filter((drawn) => drawn.scrollWidth > drawn.clientWidth)
        .map((drawn) => drawn.textContent),
    ),
  ).toEqual([]);

  // A held cell does not scroll, and neither does its editor.
  await page.evaluate(() => scrollTo(300, 250));
  await cell("Work", "A20").click();
  await expect(bar.getByRole("status")).toHaveText("Work!A20");
  await exact();
  await page.evaluate(() => scrollBy(40, 0));
  await exact();
  await cell("Work", "D1").click();
  await exact();
  await page.evaluate(() => scrollBy(0, 60));
  await exact();

  // What is typed stays through a change of the app's look.
  await cell("Work", "D30").click();
  await app.keyboard.type("half");
  const ground = () =>
    page.evaluate(() =>
      document.documentElement.style.getPropertyValue("--sheet-ground"),
    );
  const light = await ground();
  await app.emulateMedia({ colorScheme: "dark" });
  await expect.poll(ground).not.toBe(light);
  await expect(stage.locator("input.sheet-editor")).toHaveValue("half");
  await exact(2);
  await app.keyboard.press("Enter");
  await expect(cell("Work", "D30")).toHaveText("half");

  // At its end the grid grows a pane and is drawn there. The far end is
  // reached with a key; the grid is no longer for it afterwards.
  const high = () =>
    page.evaluate(() => document.querySelector(".sheet-area").offsetHeight);
  const short = await high();
  // A fit of the grid before the scroll is heard takes nothing from it.
  await page.evaluate(() => {
    scrollTo(0, document.documentElement.scrollHeight);
    dispatchEvent(new Event("resize"));
  });
  await expect.poll(high).toBeGreaterThan(short);
  await page.evaluate(() => scrollBy(0, 300));
  await expect
    .poll(() =>
      page.evaluate(
        () =>
          !!document
            .elementFromPoint(innerWidth / 2, innerHeight - 30)
            ?.closest("td"),
      ),
    )
    .toBe(true);
  await page.evaluate(() => scrollTo(0, 0));
  await cell("Work", "B2").click();
  await app.keyboard.press("Control+ArrowDown");
  await app.keyboard.press("Control+ArrowDown");
  await expect(bar.getByRole("status")).toHaveText("Work!B1048576");
  await exact();
  await app.keyboard.press("Control+Home");
  await expect(bar.getByRole("status")).toHaveText("Work!A1");
  await expect.poll(high).toBeLessThan(short * 3);

  // Alone, in the page's own type, the chart lies on its row all the same.
  const alone = await openAlone(app, files);
  await expect(alone.locator(".sheet-chart")).toBeVisible();
  expect(
    await alone.evaluate(() => {
      const top = (selector) =>
        Math.round(
          document.querySelector(selector).getBoundingClientRect().top,
        );
      return top(".sheet-chart") - top("tbody tr:nth-child(14)");
    }),
  ).toBe(0);
});

test("the grid reaches the spreadsheet's last row whatever a browser can scroll: its scroll area is a stretch of rows that moves under the view", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, bar, cell } = await openSheet(
    app,
    session,
    host.project,
    PLAN.replace('data-sheet="Tasks"', 'data-sheet="Tasks" data-frozen="1 A"'),
  );
  const page = stage.locator("html");
  await expect(cell("Tasks", "B2")).toBeVisible();
  // A stretch of ten thousand rows, where the page's own is millions of px.
  const high = await cell("Tasks", "B2").evaluate(
    (drawn) => drawn.parentElement.offsetHeight,
  );
  const span = 10_000 * high;
  await page.evaluate(
    (_, span) =>
      document.documentElement.style.setProperty("--sheet-span", `${span}`),
    span,
  );
  const area = () =>
    page.evaluate(() => document.querySelector(".sheet-area").offsetHeight);
  const placed = () => page.evaluate(() => scrollY);
  // The row under a place near the view's foot.
  const under = () =>
    page.evaluate(() =>
      Number(
        document.elementFromPoint(20, innerHeight - 40)?.closest("tr")?.cells[0]
          .dataset.row,
      ),
    );
  // The last row, by a key: selected, in view, its editor on it, and the
  // area no longer than the stretch.
  await cell("Tasks", "B2").click();
  for (let press = 0; press < 3; press++)
    await app.keyboard.press("Control+ArrowDown");
  await expect(bar.getByRole("status")).toHaveText("Tasks!B1048576");
  await expect(cell("Tasks", "B1048576")).toBeInViewport();
  expect(await area()).toBe(span);
  const boxes = () =>
    page.evaluate(() =>
      ["input.sheet-editor", 'table:not([hidden]) [aria-selected="true"]'].map(
        (selector) => {
          const box = document.querySelector(selector).getBoundingClientRect();
          return [box.left, box.top, box.width, box.height].join();
        },
      ),
    );
  const [editor, active] = await boxes();
  expect(editor).toBe(active);

  // Scrolled a long way up and down again, the stretch moves under the
  // view each time a scroll rests near its end, and the view stays on its
  // rows: each scroll moves it by just the rows scrolled.
  const rows = 3_900;
  let row = await under();
  expect(row).toBeGreaterThan(1_048_500);
  for (const way of [-1, -1, -1, -1, -1, -1, 1, 1, 1]) {
    await page.evaluate((_, by) => scrollBy(0, by), way * rows * high);
    row += way * rows;
    await expect.poll(under).toBe(row);
    // Rested: the view is in the stretch's middle half again.
    await expect
      .poll(async () => Math.abs((await placed()) - span / 2) <= span / 4)
      .toBe(true);
    expect(await under()).toBe(row);
    expect(await area()).toBe(span);
  }

  // And back to the table, where the area is as long as the grid again.
  await app.keyboard.press("Control+Home");
  await expect(bar.getByRole("status")).toHaveText("Tasks!A1");
  await page.evaluate(() => scrollTo(0, 0));
  await expect(cell("Tasks", "B2")).toBeInViewport();
  expect(await area()).toBeLessThan(span);
});

// The notice stands last in the tools' row and takes the room they leave.
for (const [pane, phone] of [
  ["a docked pane", false],
  ["a phone's pane", true],
])
  test(`a sheet's tools keep their places when a notice comes, short or long, in ${pane}`, async ({
    page: app,
    session,
    host,
  }) => {
    const { files, cell } = await openSheet(app, session, host.project, PLAN, {
      phone,
    });
    const tools = files.locator(".page-tools").first();
    const notice = tools.locator(".page-status");
    const places = () =>
      tools.getByRole("button").evaluateAll((all) =>
        all.map((tool) => {
          const { left, top, width, height } = tool.getBoundingClientRect();
          return [tool.textContent || tool.ariaLabel, left, top, width, height];
        }),
      );
    await expect(cell("Tasks", "B2")).toBeVisible();
    await expect(notice).toHaveCount(0);
    const bare = await places();
    expect(bare.map(([name]) => name)).toContain("Open alone");
    await cell("Tasks", "A2").click();
    await app.keyboard.type("Draft");
    await app.keyboard.press("Enter");
    await expect(notice).toHaveText("Saved");
    expect(await places()).toEqual(bare);
    // A refusal is longer than the row: cut short, whole in its title.
    for (let press = 0; press < 3; press++)
      await app.keyboard.press("Control+ArrowDown");
    await app.keyboard.type("x");
    await app.keyboard.press("Enter");
    const why =
      "A1048576 is too far from the table to keep: the file would hold every row between.";
    await expect(notice).toHaveText(why);
    await expect(notice).toHaveAttribute("title", why);
    expect(await places()).toEqual(bare);
    const row = await tools.boundingBox();
    const said = await notice.boundingBox();
    expect(said.x + said.width).toBeLessThanOrEqual(row.x + row.width);
  });

test("opened alone a sheet is its whole table and nothing past it", async ({
  page: app,
  session,
  host,
}) => {
  const { files } = await openSheet(
    app,
    session,
    host.project,
    sheet("Big", {
      Data: Array.from({ length: 300 }, (_, row) =>
        row ? `<td>Row ${row + 1}<td>${row}` : "<th>Name<th>Value",
      ),
    }),
  );
  const alone = await openAlone(app, files);
  await expect(alone.locator("tbody tr")).toHaveCount(300);
  await expect(alone.locator(".sheet-letters")).toHaveText("AB");
  await expect(alone.getByText("Row 300", { exact: true })).toBeVisible();
});

test("tabs are added, renamed, duplicated, moved and deleted from the bar, and the file follows each", async ({
  page: app,
  session,
  host,
}) => {
  const { files, tabs, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  const shown = files.getByRole("tab", { selected: true });
  const menu = async (name) => {
    await files.getByRole("button", { name: "Tab actions" }).click();
    await app.getByRole("menuitem", { name, exact: true }).click();
  };
  // A new tab is the one shown, and an empty table in the file.
  await files.getByRole("button", { name: "Add tab" }).click();
  await expect(tabs).toHaveText(["Tasks", "Inputs", "Sheet"]);
  await expect(shown).toHaveText("Sheet");
  const added = `${PLAN}<table data-sheet="Sheet">\n</table>\n`;
  await expect.poll(saved).toBe(added);
  await cell("Sheet", "A1").click();
  await app.keyboard.type("x");
  await app.keyboard.press("Enter");
  const typed = added.replace('"Sheet">\n', '"Sheet">\n<tr><td>x\n');
  await expect.poll(saved).toBe(typed);
  // A new name is written into the formulas that read the tab.
  await tabs.nth(0).click();
  await menu("Rename…");
  const naming = app.getByRole("dialog", { name: "Rename tab" });
  await naming.getByRole("textbox", { name: "Name" }).fill("Jobs");
  await naming.getByRole("button", { name: "Rename", exact: true }).click();
  await expect(tabs).toHaveText(["Jobs", "Inputs", "Sheet"]);
  const named = typed
    .replace('data-sheet="Tasks"', 'data-sheet="Jobs"')
    .replace("=Tasks!D5", "=Jobs!D5");
  await expect.poll(saved).toBe(named);
  // A copy comes after its tab and is the one shown; moved, it stays so.
  await menu("Duplicate");
  await expect(tabs).toHaveText(["Jobs", "Jobs copy", "Inputs", "Sheet"]);
  await expect(shown).toHaveText("Jobs copy");
  await menu("Move right");
  await expect(tabs).toHaveText(["Jobs", "Inputs", "Jobs copy", "Sheet"]);
  await expect(shown).toHaveText("Jobs copy");
  await expect(cell("Jobs copy", "A2")).toHaveText("Write");
  await expect
    .poll(async () => (await saved()).match(/data-sheet="[^"]+"/g))
    .toEqual(
      ["Jobs", "Inputs", "Jobs copy", "Sheet"].map(
        (name) => `data-sheet="${name}"`,
      ),
    );
  // Deleting asks first and says who reads the tab; they show an error.
  await tabs.nth(0).click();
  await menu("Delete…");
  const asking = app.getByRole("dialog", { name: "Delete tab" });
  await expect(asking).toContainText("1 cell of other tabs reads it");
  await asking.getByRole("button", { name: "Delete tab", exact: true }).click();
  await expect(tabs).toHaveText(["Inputs", "Jobs copy", "Sheet"]);
  await expect(cell("Inputs", "B2")).toHaveClass(/sheet-error/);
  await expect.poll(saved).not.toContain('data-sheet="Jobs"');
  // Undo brings the tab back, in the file too.
  await files.getByRole("button", { name: "Undo" }).click();
  await expect(tabs).toHaveText(["Jobs", "Inputs", "Jobs copy", "Sheet"]);
  await expect.poll(saved).toContain('data-sheet="Jobs"');
});

test("a cell's menu shows one value alone or leaves it out, holds rows and columns up to the cell, pastes values and writes a link", async ({
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
  const pick = async (ref, name) => {
    await cell("Tasks", ref).click({ button: "right" });
    await stage.getByRole("menuitem", { name, exact: true }).click();
  };
  const note = files.locator(".sheet-note");
  // The total row is never filtered out.
  await pick("B3", "Show only “To do”");
  await expect(note).toHaveText("2 rows hidden");
  await expect(
    cell("Tasks", "B1").getByRole("button", { pressed: true }),
  ).toBeVisible();
  await pick("B3", "Clear filter");
  await expect(note).toHaveCount(0);
  await pick("B3", "Leave out “To do”");
  await expect(note).toHaveText("1 row hidden");
  await pick("B2", "Clear filter");
  // Held up to the cell, then let go; each is a step.
  const frozen = (said) =>
    said
      ? PLAN.replace(
          'data-sheet="Tasks"',
          `data-sheet="Tasks" data-frozen="${said}"`,
        )
      : PLAN;
  await pick("B3", "Freeze rows up to here");
  await expect.poll(saved).toBe(frozen("3"));
  await pick("B3", "Freeze columns up to here");
  await expect.poll(saved).toBe(frozen("3 B"));
  await expect(cell("Tasks", "B3")).toHaveClass(/sheet-pinned/);
  await pick("B4", "Unfreeze");
  await expect.poll(saved).toBe(frozen("0"));
  for (let step = 0; step < 3; step++)
    await files.getByRole("button", { name: "Undo" }).click();
  await expect.poll(saved).toBe(PLAN);
  // What a formula shows is pasted, not the formula.
  const run = await clipboard(stage);
  await cell("Tasks", "E2").click();
  await run("copy");
  await pick("A7", "Paste values only");
  await expect(cell("Tasks", "A7")).toHaveText("2,400.00");
  await expect.poll(saved).toContain("\n<tr><td>2,400.00<td><td><td><td>\n");
  // A link is a formula of the address and what the cell showed.
  await cell("Tasks", "A2").click({ button: "right" });
  const link = stage.getByRole("searchbox", { name: "Link to an address" });
  await link.fill("https://example.com/a");
  await link.press("Enter");
  await expect(cell("Tasks", "A2")).toHaveText("Write");
  await expect(cell("Tasks", "A2")).toHaveClass(/sheet-link/);
  await expect
    .poll(saved)
    .toContain('<td>=HYPERLINK("https://example.com/a","Write")<td>');
});

test("the selection's cells are made bold, coloured, filled, aligned, patterned and wrapped from the formula bar, which shows how the active cell is", async ({
  page: app,
  session,
  host,
}) => {
  const narrow = with_(PLAN, {
    "<col><col data-options": '<col data-width="60"><col data-options',
  });
  const { files, stage, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    narrow,
  );
  const bar = files.getByRole("group", { name: "Formula bar" });
  const press = (name) => bar.getByRole("button", { name, exact: true });
  const choice = (name) =>
    app.getByRole("menuitemradio", { name, exact: true });
  const choose = async (menu, name) => {
    await press(menu).click();
    await choice(name).click();
  };
  const undo = files.getByRole("button", { name: "Undo" });
  // A word of the class: on where a cell lacks it, off where all have it.
  await cell("Tasks", "A2").click();
  await expect(press("Bold")).toHaveAttribute("aria-pressed", "false");
  await press("Bold").click();
  await expect(cell("Tasks", "A2")).toHaveClass(/sheet-bold/);
  await expect(press("Bold")).toHaveAttribute("aria-pressed", "true");
  await cell("Tasks", "A2").click();
  await app.keyboard.press("Shift+ArrowDown");
  await press("Bold").click();
  await press("Italic").click();
  const marked = with_(narrow, {
    "<td>Write": '<td class="bold italic">Write',
    "<td>Book": '<td class="bold italic">Book',
  });
  await expect.poll(saved).toBe(marked);
  await expect(cell("Tasks", "A3")).toHaveCSS("font-style", "italic");
  await press("Bold").click();
  await press("Italic").click();
  await expect.poll(saved).toBe(narrow);
  await expect(cell("Tasks", "A2")).not.toHaveClass(/sheet-bold/);
  // In the grid the keys do the same.
  await cell("Tasks", "A2").click();
  await app.keyboard.press("Control+b");
  await app.keyboard.press("Control+i");
  await expect(press("Bold")).toHaveAttribute("aria-pressed", "true");
  await expect
    .poll(saved)
    .toBe(with_(narrow, { "<td>Write": '<td class="bold italic">Write' }));
  await app.keyboard.press("Control+i");
  await app.keyboard.press("Control+b");
  await expect.poll(saved).toBe(narrow);
  // A header and a total are bold whatever is pressed, and are shown so.
  await cell("Tasks", "A1").click();
  await expect(press("Bold")).toHaveAttribute("aria-pressed", "true");

  // A fill is the one its menu shows, over a header's band too; the menu
  // ticks the one the cell has.
  const ground = (ref) =>
    cell("Tasks", ref).evaluate(
      (drawn) => getComputedStyle(drawn).backgroundColor,
    );
  await press("Fill").click();
  await expect(choice("None")).toHaveAttribute("aria-checked", "true");
  const good = await choice("Good")
    .locator("span")
    .evaluate((name) => getComputedStyle(name, "::before").backgroundColor);
  await choice("Good").click();
  await expect.poll(() => ground("A1")).toBe(good);
  expect(await ground("B1")).not.toBe(good);
  await press("Fill").click();
  await expect(choice("Good")).toHaveAttribute("aria-checked", "true");
  await app.keyboard.press("Escape");
  await cell("Tasks", "D3").click();
  const ink = () =>
    cell("Tasks", "D3").evaluate((drawn) => getComputedStyle(drawn).color);
  const plain = await ink();
  await choose("Text colour", "Bad");
  await expect.poll(ink).not.toBe(plain);

  // A number stands right until it is told where; its own pattern stands
  // over its column's.
  await press("Align").click();
  await expect(choice("Right")).toHaveAttribute("aria-checked", "true");
  await choice("Center").click();
  await expect(cell("Tasks", "D3")).toHaveCSS("text-align", "center");
  await choose("Number format", "Currency");
  await expect(cell("Tasks", "D3")).toHaveText("$8,400.50");
  const looked = with_(narrow, {
    "<th>Task": '<th data-fill="good">Task',
    "<td>8400.5":
      '<td data-ink="bad" class="center" data-format="$#,##0.00">8400.5',
  });
  await expect.poll(saved).toBe(looked);
  await choose("Number format", "General");
  await expect(cell("Tasks", "D3")).toHaveText("8400.5");
  await press("Number format").click();
  await expect(choice("General")).toHaveAttribute("aria-checked", "true");
  await app.keyboard.press("Escape");
  await undo.click();
  await expect(cell("Tasks", "D3")).toHaveText("$8,400.50");

  // Wrapped, a long text takes the lines its column leaves it.
  await cell("Tasks", "A4").click();
  await app.keyboard.type("Brief the whole team");
  await app.keyboard.press("Enter");
  const high = () =>
    cell("Tasks", "A4").evaluate((drawn) => drawn.parentElement.offsetHeight);
  const line = await high();
  await cell("Tasks", "A4").click();
  await press("Wrap text").click();
  await expect.poll(high).toBeGreaterThan(line * 2);
  await expect(press("Wrap text")).toHaveAttribute("aria-pressed", "true");
  await expect.poll(saved).toContain('<td class="wrap">Brief the whole team');
  await undo.click();
  await undo.click();
  await expect.poll(saved).toBe(looked);

  // Only the rows in view take a look; past the table it reaches there.
  await cell("Tasks", "B2").click({ button: "right" });
  await stage.getByRole("menuitem", { name: "Show only “Done”" }).click();
  await cell("Tasks", "A2").click();
  await app.keyboard.press("Shift+ArrowDown");
  await expect(bar.getByRole("status")).toHaveText("Tasks!A2");
  await choose("Fill", "Warning");
  await expect.poll(saved).toBe(
    with_(looked, {
      "<td>Write": '<td data-fill="warn">Write',
      "<td>Total": '<td data-fill="warn">Total',
    }),
  );
  await undo.click();
  await cell("Tasks", "B2").click({ button: "right" });
  await stage.getByRole("menuitem", { name: "Clear filter" }).click();
  await cell("Tasks", "G7").click();
  await choose("Fill", "Accent");
  await expect
    .poll(saved)
    .toContain(
      '\n<tr><td><td><td><td><td><td><td data-fill="accent">\n</table>',
    );
  await expect(cell("Tasks", "G7")).toHaveAttribute("data-fill", "accent");
  await undo.click();
  await expect.poll(saved).toBe(looked);

  // A narrow pane folds the controls into one sheet.
  await app.setViewportSize({ width: 390, height: 780 });
  await expect(press("Bold")).toBeHidden();
  await cell("Tasks", "A2").click();
  await press("Format").click();
  const folded = app.getByRole("dialog", { name: "Format" });
  await folded.getByRole("button", { name: "Bold" }).click();
  await expect(folded.getByRole("button", { name: "Bold" })).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await folded.getByRole("button", { name: "Fill" }).click();
  await choice("Bad").click();
  await expect
    .poll(saved)
    .toBe(
      with_(looked, { "<td>Write": '<td class="bold" data-fill="bad">Write' }),
    );
});

test("a row's number and a column's letter open what can be done to them: move, freeze, clear, format, copy and comment; a column's edge is dragged to its width", async ({
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
  await expect(cell("Tasks", "A2")).toBeVisible();
  const shown = "table:not([hidden]) ";
  const letter = (column) =>
    stage.locator(`${shown}.sheet-letters [data-column="${column}"]`);
  const number = (row) =>
    stage.locator(`${shown}tbody th.sheet-edge[data-row="${row}"]`);
  const item = (name, role = "menuitem") =>
    stage.getByRole(role, { name, exact: true });
  const undo = files.getByRole("button", { name: "Undo" });

  // A column moves among its neighbours with its pattern; what read its
  // cells reads them there, a range down it and the other tab too.
  await letter(4).click();
  await expect(bar.getByRole("status")).toHaveText("Tasks!D1");
  await item("Move right").click();
  const columned = with_(PLAN, {
    '<col data-format="#,##0.00"><col>': '<col><col data-format="#,##0.00">',
    "<th>Cost<th>Double": "<th>Double<th>Cost",
    "<td>1200<td>=D2*2": "<td>=E2*2<td>1200",
    "<td>8400.5<td>=D3*2": "<td>=E3*2<td>8400.5",
    "<td>300<td>=D4*2": "<td>=E4*2<td>300",
    "=Tasks!D5": "=Tasks!E5",
  });
  await expect.poll(saved).toBe(columned);
  await expect(bar.getByRole("status")).toHaveText("Tasks!E1");
  await expect(cell("Tasks", "D5")).toHaveText("19,801.00");
  await undo.click();
  await expect.poll(saved).toBe(PLAN);

  // A row moves under the header only; the same list comes by the other
  // button of the mouse.
  await number(3).click({ button: "right" });
  await item("Move down").click();
  const rowed = with_(PLAN, {
    "<tr><td>Book<td>To do<td>FALSE<td>8400.5<td>=D3*2\n<tr><td>Brief<td>In progress<td>FALSE<td>300<td>=D4*2":
      "<tr><td>Brief<td>In progress<td>FALSE<td>300<td>=D3*2\n<tr><td>Book<td>To do<td>FALSE<td>8400.5<td>=D4*2",
  });
  await expect.poll(saved).toBe(rowed);
  await expect(bar.getByRole("status")).toHaveText("Tasks!A4");
  await undo.click();
  await number(2).click();
  await item("Move up").click();
  await expect(files.locator(".page-tools .notice")).toHaveText(
    "A header row keeps its place.",
  );
  expect(await saved()).toBe(PLAN);

  // Held up to a row or a column, and let go, from their own menus.
  const frozen = (said) =>
    PLAN.replace(
      'data-sheet="Tasks"',
      `data-sheet="Tasks" data-frozen="${said}"`,
    );
  await number(2).click();
  await item("Freeze rows up to here").click();
  await expect.poll(saved).toBe(frozen("2"));
  await letter(2).click();
  await item("Freeze columns up to here").click();
  await expect.poll(saved).toBe(frozen("2 B"));
  await letter(3).click();
  await item("Unfreeze").click();
  await expect.poll(saved).toBe(frozen("0"));
  for (let step = 0; step < 3; step++) await undo.click();
  await expect.poll(saved).toBe(PLAN);

  // The format row's looks from the menu, ticked where the cell has one;
  // cleared, a column selected whole gives up its own pattern too.
  await letter(4).click();
  await item("Formatting").click();
  await item("Fill").click();
  await expect(item("None", "menuitemcheckbox")).toHaveAttribute(
    "aria-checked",
    "true",
  );
  await item("Good", "menuitemcheckbox").click();
  await expect(cell("Tasks", "D3")).toHaveAttribute("data-fill", "good");
  await letter(4).click();
  await item("Formatting").click();
  await item("Bold", "menuitemcheckbox").click();
  await expect(cell("Tasks", "D3")).toHaveClass(/sheet-bold/);
  await letter(4).click();
  await item("Clear formatting").click();
  await expect(cell("Tasks", "D3")).not.toHaveAttribute("data-fill");
  await expect(cell("Tasks", "D3")).toHaveText("8400.5");
  await expect
    .poll(saved)
    .toBe(with_(PLAN, { '<col data-format="#,##0.00">': "<col>" }));
  for (let step = 0; step < 3; step++) await undo.click();
  await expect.poll(saved).toBe(PLAN);

  // A column's edge dragged is its width in the file; fitted to its texts
  // again from the menu, or by pressing the edge twice.
  const edge = letter(1).locator(".sheet-resize");
  const wide = () =>
    letter(1).evaluate((drawn) => drawn.getBoundingClientRect().width);
  const fitted = await wide();
  const at = await edge.boundingBox();
  await app.mouse.move(at.x + 3, at.y + 5);
  await app.mouse.down();
  await app.mouse.move(at.x + 63, at.y + 5, { steps: 3 });
  await expect(stage.locator(".sheet-guide")).toBeVisible();
  await app.mouse.up();
  const sized = with_(PLAN, {
    "<col><col data-options": `<col data-width="${fitted + 60}"><col data-options`,
  });
  await expect.poll(saved).toBe(sized);
  await expect.poll(wide).toBe(fitted + 60);
  await letter(1).click();
  await item("Fit to content").click();
  await expect.poll(saved).toBe(PLAN);
  await undo.click();
  await expect.poll(saved).toBe(sized);
  await edge.dblclick();
  await expect.poll(saved).toBe(PLAN);

  // A row copied from its menu is pasted from a cell's, formulas and all.
  await number(2).click();
  await expect(item("Paste")).toHaveCount(0);
  await item("Copy").click();
  await cell("Tasks", "A7").click({ button: "right" });
  await item("Paste").click();
  await expect(cell("Tasks", "A7")).toHaveText("Write");
  await expect(cell("Tasks", "E7")).toHaveText("2,400.00");
  await expect
    .poll(saved)
    .toContain("\n<tr><td>Write<td>Done<td>TRUE<td>1200<td>=D7*2\n</table>");

  // A comment on the row, from its menu.
  await number(3).click();
  await item("Comment").click();
  await expect(files.getByText("Comment on Tasks!A3:E3")).toBeVisible();
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
  await cell("Months", "B2").click();
  await app.keyboard.press("Control+End");
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
  const { files, stage, bar, cell, saved } = await openSheet(
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

  // A click picks the chart: its tools and corners show, and the cells it
  // reads are tinted. Escape, or a press on a cell, lets go of it.
  const box = stage.locator(".sheet-chart");
  const tools = box.getByRole("button");
  await expect(tools).toHaveCount(0);
  await box.locator("figcaption").click();
  await expect(tools).toHaveText(["Edit", "Delete"]);
  await expect(box.locator(".sheet-size")).toHaveCount(4);
  await expect(stage.locator(".sheet-read")).toHaveCount(12);
  await app.keyboard.press("Escape");
  await expect(tools).toHaveCount(0);
  await expect(stage.locator(".sheet-read")).toHaveCount(0);
  await box.locator("figcaption").click();
  await cell("Costs", "A2").click();
  await expect(box.locator(".sheet-size")).toHaveCount(0);
  // Edit changes its kind.
  await box.locator("figcaption").click();
  await stage.getByRole("button", { name: /^Edit chart/ }).click();
  await stage.getByRole("menuitem", { name: "Pie" }).click();
  await expect(chart.locator("path")).toHaveCount(3);
  const pie = with_(charted, {
    "<td>90<td>": "<td>2400<td>",
    [costs]: figure("pie", "A1:C4", "E2", "Costs per item"),
  });
  await expect.poll(saved).toBe(pie);

  // A chart of the selection, put in beside it.
  await app.keyboard.press("Control+Home");
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

  // Dragged, the chart sits on the cell under its corner. Picked, a corner
  // sizes it, in cells of 96 by 24 wherever it lies: the one at its bottom
  // right leaves its place, the one at its top left moves that too.
  await drag(app, stage.locator(".sheet-chart figcaption"), 200, 5);
  await expect.poll(saved).toContain('data-at="G2" data-size="6x12"');
  const corner = (name) => stage.locator(`.sheet-size[data-corner="${name}"]`);
  await box.locator("figcaption").click();
  await drag(app, corner("se"), 110, 30);
  await expect.poll(saved).toContain('data-at="G2" data-size="7x13"');
  await drag(app, corner("nw"), 96, 52);
  await expect.poll(saved).toContain('data-at="H4" data-size="6x11"');

  // The file's chart is there when the sheet is read anew; deleted, it is
  // gone from the file.
  await stage.locator("html").evaluate(() => location.reload());
  await expect(chart.locator("path")).toHaveCount(3);
  await box.locator("figcaption").click();
  const sized = await saved();
  const gone = with_(pie, {
    [`\n${figure("pie", "A1:C4", "E2", "Costs per item")}`]: "",
  });
  await stage.getByRole("button", { name: /^Delete chart/ }).click();
  await expect(stage.locator(".sheet-chart")).toHaveCount(0);
  await expect.poll(saved).toBe(gone);

  // Picked, the chart is what Delete takes out, not the cells selected
  // under it; any other key lets go of it and is the cells'.
  const undo = files.getByRole("button", { name: "Undo" });
  await undo.click();
  await expect.poll(saved).toBe(sized);
  await cell("Costs", "A2").click();
  await box.locator("figcaption").click();
  await expect(box).toHaveAttribute("data-picked", "");
  await app.keyboard.press("Delete");
  await expect(stage.locator(".sheet-chart")).toHaveCount(0);
  await expect.poll(saved).toBe(gone);
  await undo.click();
  await box.locator("figcaption").click();
  await expect(box).toHaveAttribute("data-picked", "");
  await app.keyboard.press("ArrowDown");
  await expect(box).not.toHaveAttribute("data-picked");
  await expect(bar.getByRole("status")).toHaveText("Costs!A3");
  await expect.poll(saved).toBe(sized);
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
