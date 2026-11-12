// A column's conditional formatting is set by a person in a dialog: the
// rules the file holds, in words, and what is set there written as the
// agent writes it.
import { test, expect, openSheet, sheet } from "./fixtures.js";

// Costs with a rule of their own; the table strikes the rows that are done
// and holds a rule that is not understood.
const TABLE = "B = Done: strike muted; B = Nope: sparkle";
const PLAN = sheet("Plan", {
  Tasks: [
    '<col><col><col data-rule="< 0: bad; > 1000: bold">',
    "<th>Task<th>Status<th>Cost",
    "<td>Write<td>Done<td>1200",
    "<td>Book<td>To do<td>-5",
    "<td>Brief<td>To do<td>300",
  ],
}).replace('data-sheet="Tasks"', `data-sheet="Tasks" data-rule="${TABLE}"`);

test("a column's rules are listed in words; one is added, changed and taken out, each a step that Undo takes back", async ({
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
  const dialog = app.getByRole("dialog", {
    name: "Conditional formatting: column C",
  });
  const rows = dialog.locator(".row");
  const button = (name) => dialog.getByRole("button", { name, exact: true });
  const column = (rules) =>
    PLAN.replace('"< 0: bad; > 1000: bold"', `"${rules}"`);
  // From a cell's menu: the rules of its column, as the file has them.
  await cell("Tasks", "C4").click({ button: "right" });
  await stage
    .getByRole("menuitem", { name: "Conditional formatting…" })
    .click();
  await expect(rows).toHaveText([
    /^Is less than 0Bad fill on the cell/,
    /^Is greater than 1000Bold on the cell/,
  ]);

  // A rule is saved once it tests something and gives something.
  await button("Add rule").click();
  const save = button("Save");
  const value = dialog.getByRole("textbox", { name: "Value" });
  await expect(save).toBeDisabled();
  await dialog.getByLabel("When the cell").selectOption("Is at least");
  await dialog.getByLabel("Fill").selectOption("Warning");
  await expect(save).toBeDisabled();
  // More or less is of an amount.
  await value.fill("many");
  await expect(save).toBeDisabled();
  await value.fill("100");
  await expect(save).toBeEnabled();
  // A condition without a value asks for none.
  await dialog.getByLabel("When the cell").selectOption("Is blank");
  await expect(value).toHaveCount(0);
  await expect(save).toBeEnabled();
  await dialog.getByLabel("When the cell").selectOption("Is at least");
  await expect(value).toHaveValue("100");
  await save.click();
  // The page says the rules again, the grid shows them and the file has them.
  await expect(rows).toHaveText([
    /^Is less than 0/,
    /^Is greater than 1000/,
    /^Is at least 100Warning fill on the cell/,
  ]);
  await expect(cell("Tasks", "C4")).toHaveAttribute("data-fill", "warn");
  await expect(cell("Tasks", "C3")).toHaveAttribute("data-fill", "bad");
  await expect.poll(saved).toBe(column("< 0: bad; > 1000: bold; >= 100: warn"));

  // Edit shows the rule as it is; saved, it stands where it stood.
  await button("Edit Is greater than 1000").click();
  await expect(dialog.getByLabel("When the cell")).toHaveValue(">");
  await expect(value).toHaveValue("1000");
  await expect(dialog.getByRole("switch", { name: "Bold" })).toBeChecked();
  await value.fill("1100");
  await dialog.getByRole("switch", { name: "Strike" }).check();
  await save.click();
  await expect(rows.nth(1)).toHaveText(
    /^Is greater than 1100Bold, strike on the cell/,
  );
  await expect(cell("Tasks", "C2")).toHaveClass(/sheet-strike/);
  await expect
    .poll(saved)
    .toBe(column("< 0: bad; > 1100: bold strike; >= 100: warn"));
  // Cancel leaves it.
  await button("Edit Is less than 0").click();
  await dialog.getByLabel("Fill").selectOption("Good");
  await button("Cancel").click();
  await expect(rows.first()).toHaveText(/Bad fill on the cell/);

  await button("Remove Is less than 0").click();
  await expect(rows).toHaveCount(2);
  await expect(cell("Tasks", "C3")).not.toHaveAttribute("data-fill");
  await expect.poll(saved).toBe(column("> 1100: bold strike; >= 100: warn"));
  await button("Done").click();
  await expect(dialog).toBeHidden();
  // Three changes, three steps.
  const undo = files.getByRole("button", { name: "Undo" });
  for (const _ of "123") await undo.click();
  await expect(undo).toBeDisabled();
  await expect.poll(saved).toBe(PLAN);
});

test("a rule on the whole row is set from the column it tests; one not understood is shown as written and can only be removed", async ({
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
  const dialog = app.getByRole("dialog", { name: /^Conditional formatting/ });
  const rows = dialog.locator(".row");
  const button = (name) => dialog.getByRole("button", { name, exact: true });
  const table = (rules) => PLAN.replace(`"${TABLE}"`, `"${rules}"`);
  // From a column's own menu.
  await stage
    .locator('table[data-sheet="Tasks"] .sheet-letters [data-column="2"]')
    .click();
  await stage
    .getByRole("menuitem", { name: "Conditional formatting…" })
    .click();
  await expect(dialog).toHaveAccessibleName("Conditional formatting: column B");
  await expect(rows).toHaveText([
    /^Is DoneMuted fill, strike on the whole row/,
    /^= Nope: sparkleNot understood, on the whole row/,
  ]);
  await expect(button("Edit = Nope: sparkle")).toHaveCount(0);
  await expect(files.locator(".page-notice")).toContainText(/^1 cell in error/);
  await button("Remove = Nope: sparkle").click();
  await expect(rows).toHaveCount(1);
  await expect.poll(saved).toBe(table("B = Done: strike muted"));
  await expect(files.locator(".page-notice")).toHaveCount(0);

  // A text to compare may hold what a file must escape; a semicolon ends
  // a rule, so it is not taken.
  await button("Add rule").click();
  const value = dialog.getByRole("textbox", { name: "Value" });
  await dialog.getByRole("switch", { name: "Bold" }).check();
  await dialog.getByRole("switch", { name: "The whole row" }).check();
  await value.fill("To do; later");
  await expect(button("Save")).toBeDisabled();
  await value.fill('To "do" & more');
  await expect(button("Save")).toBeEnabled();
  await value.fill("To do");
  await button("Save").click();
  await expect(rows).toHaveText([/^Is Done/, /^Is To doBold on the whole row/]);
  for (const ref of ["A3", "C4"])
    await expect(cell("Tasks", ref)).toHaveClass(/sheet-bold/);
  await expect(cell("Tasks", "A2")).not.toHaveClass(/sheet-bold/);
  await expect
    .poll(saved)
    .toBe(table("B = Done: strike muted; B = To do: bold"));

  // Edited to be on the cell alone, it is the column's own.
  await button("Edit Is To do").click();
  await dialog.getByRole("switch", { name: "The whole row" }).uncheck();
  await button("Save").click();
  await expect(rows.last()).toHaveText(/^Is To doBold on the cell/);
  await expect(cell("Tasks", "B3")).toHaveClass(/sheet-bold/);
  await expect(cell("Tasks", "A3")).not.toHaveClass(/sheet-bold/);
  await expect
    .poll(saved)
    .toBe(
      table("B = Done: strike muted").replace(
        "<col><col><col data-rule",
        '<col><col data-rule="= To do: bold"><col data-rule',
      ),
    );
});

test("a column without rules says so", async ({ page: app, session, host }) => {
  const { stage, cell } = await openSheet(app, session, host.project, PLAN);
  await cell("Tasks", "A2").click({ button: "right" });
  await stage
    .getByRole("menuitem", { name: "Conditional formatting…" })
    .click();
  const dialog = app.getByRole("dialog", {
    name: "Conditional formatting: column A",
  });
  await expect(dialog.getByRole("status")).toHaveText(
    "No rule formats this column.",
  );
  await dialog.getByRole("button", { name: "Done" }).click();
  await expect(dialog).toBeHidden();
});

test("a file that is only read offers no rules to set", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, cell } = await openSheet(
    app,
    session,
    host.project,
    "Item,Cost\nA,1\n",
    { path: "data/costs.csv" },
  );
  await cell("costs", "B2").click({ button: "right" });
  await expect(
    stage.getByRole("menuitem", { name: /^Show only/ }),
  ).toBeVisible();
  await expect(
    stage.getByRole("menuitem", { name: "Conditional formatting…" }),
  ).toHaveCount(0);
});

test("the keys follow the dialog: into the form and back to what opened it, and the pane under it takes none", async ({
  page: app,
  session,
  host,
}) => {
  const two = PLAN.replace(
    /\n$/,
    '\n<table data-sheet="More">\n<tr><td>1\n</table>\n',
  );
  const { stage, tabs, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    two,
  );
  const dialog = app.getByRole("dialog", {
    name: "Conditional formatting: column A",
  });
  const button = (name) => dialog.getByRole("button", { name, exact: true });
  const value = dialog.getByRole("textbox", { name: "Value" });
  await cell("Tasks", "A2").click({ button: "right" });
  await stage
    .getByRole("menuitem", { name: "Conditional formatting…" })
    .click();
  // Add rule by its key: the form's first field has the keys, and what is
  // typed goes into the value.
  await button("Add rule").focus();
  await app.keyboard.press("Enter");
  await expect(dialog.getByLabel("When the cell")).toBeFocused();
  await app.keyboard.press("Tab");
  await expect(value).toBeFocused();
  await app.keyboard.type("Write");
  await expect(value).toHaveValue("Write");
  // The pane's key for the next tab is not the dialog's.
  await app.keyboard.press("Control+PageDown");
  await dialog.getByRole("switch", { name: "Bold" }).check();
  await button("Save").click();
  // Saved, the keys are at the rule; cancelled, at what opened the form.
  await expect(button("Edit Is Write")).toBeFocused();
  await expect(tabs.and(app.locator('[aria-selected="true"]'))).toHaveText(
    "Tasks",
  );
  await expect.poll(saved).toContain('<col data-rule="= Write: bold">');
  await app.keyboard.press("Enter");
  await expect(dialog.getByLabel("When the cell")).toBeFocused();
  await button("Cancel").click();
  await expect(button("Edit Is Write")).toBeFocused();
  await button("Add rule").click();
  await button("Cancel").click();
  await expect(button("Add rule")).toBeFocused();
});

test("what the page refuses is said in the form, which stays; a rule that names no column is listed with every column's; another cell's column closes the dialog", async ({
  page: app,
  session,
  host,
}) => {
  const { stage, bar, cell, saved } = await openSheet(
    app,
    session,
    host.project,
    PLAN.replace(TABLE, `sparkle; ${TABLE}`),
  );
  const dialog = app.getByRole("dialog", { name: /^Conditional formatting/ });
  const rows = dialog.locator(".row");
  const button = (name) => dialog.getByRole("button", { name, exact: true });
  // A column far past the table: it can have a rule of its own, none on
  // its rows.
  await cell("Tasks", "C2").click();
  for (let step = 0; step < 30; step++) await app.keyboard.press("ArrowRight");
  await expect(bar.locator("output")).toHaveText("Tasks!AG2");
  await app.keyboard.press("Shift+F10");
  await stage
    .getByRole("menuitem", { name: "Conditional formatting…" })
    .click();
  await expect(dialog).toHaveAccessibleName(
    "Conditional formatting: column AG",
  );
  await expect(rows).toHaveText([/^sparkleNot understood, on the whole row/]);
  await expect(button("Edit sparkle")).toHaveCount(0);
  await button("Add rule").click();
  await dialog.getByLabel("When the cell").selectOption("Is greater than");
  await dialog.getByRole("textbox", { name: "Value" }).fill("1");
  await dialog.getByLabel("Fill").selectOption("Bad");
  const whole = dialog.getByRole("switch", { name: "The whole row" });
  await whole.check();
  await button("Save").click();
  await expect(dialog.getByRole("alert")).toHaveText(
    "Not changed: column AG is too far from the table for a rule on its rows.",
  );
  await expect(whole).toBeChecked();
  await whole.uncheck();
  await button("Save").click();
  await expect(rows).toHaveText([/^sparkle/, /^Is greater than 1Bad fill/]);
  await expect(dialog.getByRole("alert")).toHaveCount(0);
  await expect.poll(saved).toContain('<col data-rule="> 1: bad">');
  await button("Remove sparkle").click();
  await expect(rows).toHaveCount(1);
  await expect.poll(saved).toContain(`data-rule="${TABLE}"`);

  // The page says another cell: the dialog was for this column, and is
  // not opened again by coming back to it.
  await cell("Tasks", "AF2").dispatchEvent("mousedown");
  await expect(dialog).toBeHidden();
  await cell("Tasks", "AG2").dispatchEvent("mousedown");
  await expect(bar.locator("output")).toHaveText("Tasks!AG2");
  await expect(dialog).toBeHidden();
});
