// A sheet's sizes, lines, row heights and coloured options as the file
// writes them, set from the format row too; and find and replace over what
// the cells show.
import { test, expect, openSheet, sheet } from "./fixtures.js";

const PLAN = sheet("Plan", {
  Tasks: [
    '<col><col data-options="To do:muted|Done:good">',
    '<tr data-height="44" class="large" data-border="bottom"><th>Task<th>Status',
    "<td>Write the plan<td>Done",
    "<td>Book the venue<td>To do",
    "<td>=UPPER(A3)<td>To do",
  ],
  Notes: ["<td>Plan the venue"],
});

test("sizes, lines, a row's height and an option's colour show as written, and the format row sets them", async ({
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
  const head = cell("Tasks", "A1");
  await expect(head).toHaveClass(/sheet-large/);
  await expect(head).toHaveAttribute("data-border", "bottom");
  await expect
    .poll(async () => (await head.boundingBox())?.height)
    .toBeGreaterThanOrEqual(44);
  // An option's colour fills its cells; the list offers the option alone.
  await expect(cell("Tasks", "B2")).toHaveAttribute("data-fill", "good");
  await expect(cell("Tasks", "B3")).toHaveAttribute("data-fill", "muted");

  await cell("Tasks", "A2").click();
  await files.getByRole("button", { name: "Borders" }).click();
  await app.getByRole("menuitemradio", { name: "All" }).click();
  await expect(cell("Tasks", "A2")).toHaveAttribute("data-border", "all");
  await files.getByRole("button", { name: "Text size" }).click();
  await app.getByRole("menuitemradio", { name: "Small" }).click();
  await expect(cell("Tasks", "A2")).toHaveClass(/sheet-small/);
  await expect
    .poll(saved)
    .toContain('<td data-border="all" class="small">Write the plan');
  await files.getByRole("button", { name: "Text size" }).click();
  await app.getByRole("menuitemradio", { name: "Normal" }).click();
  await expect.poll(saved).toContain('<td data-border="all">Write the plan');
});

test("find counts what cells show on every tab; replace changes typed text, not a formula", async ({
  page: app,
  session,
  host,
}) => {
  const { files, cell, saved, tabs } = await openSheet(
    app,
    session,
    host.project,
    PLAN,
  );
  await files.getByRole("button", { name: "Find" }).click();
  const find = files.getByRole("searchbox", { name: "Find in this sheet" });
  await find.fill("plan");
  // A2 on Tasks, A1 on Notes.
  await expect(files.locator(".find-count")).toHaveText("1 of 2");
  await expect(cell("Tasks", "A2")).toHaveAttribute("aria-selected", "true");
  // A4 shows A3's text in capitals.
  await find.fill("venue");
  await expect(files.locator(".find-count")).toHaveText("1 of 3");
  await expect(cell("Tasks", "A3")).toHaveAttribute("aria-selected", "true");

  await files.getByRole("textbox", { name: "Replace with" }).fill("hall");
  await files.getByRole("button", { name: "Replace all" }).click();
  await expect.poll(saved).toContain("<td>Book the hall<td>To do");
  await expect.poll(saved).toContain("<td>Plan the hall");
  // The formula is as it was, and shows the new text.
  await expect.poll(saved).toContain("<td>=UPPER(A3)<td>To do");
  await expect(files.locator(".find-count")).toHaveText("No matches");
  await expect(tabs).toHaveCount(2);
});
