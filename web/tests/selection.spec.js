// Several conversations picked at once in the sidebar, as a desktop file
// list picks them, and acted on together: the multi-select canvas.
import { test, expect, savedConversation } from "./fixtures.js";
import { access, mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";

async function three(host, request) {
  for (const title of ["Alpha talk", "Bravo talk", "Charlie talk"])
    await savedConversation(host, request, title);
}
const row = (page, title) =>
  page
    .getByRole("navigation", { name: "Conversations" })
    .locator(".session-row", { hasText: title });

test("⌘ picks one, Shift a run, one confirmation deletes them all", async ({
  page,
  host,
  request,
}) => {
  await three(host, request);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/");
  const list = page.getByRole("navigation", { name: "Conversations" });
  const titles = await list.locator(".session-row .row-label").allInnerTexts();
  const ours = titles.filter((title) => title.endsWith(" talk"));
  expect(ours).toHaveLength(3);
  // ⌘ or Ctrl picks without opening.
  await row(page, ours[0])
    .locator(".list-row")
    .click({ modifiers: ["ControlOrMeta"] });
  const bar = page.getByRole("toolbar", { name: "Selection" });
  await expect(bar.getByText("1 selected")).toBeVisible();
  await expect(page).not.toHaveURL(/#session=/);
  // Shift adds the run to the last, in the list's order.
  await row(page, ours[2])
    .locator(".list-row")
    .click({ modifiers: ["Shift"] });
  await expect(bar.getByText("3 selected")).toBeVisible();
  for (const title of ours)
    await expect(row(page, title).getByRole("checkbox")).toBeChecked();
  // Esc ends it.
  await page.keyboard.press("Escape");
  await expect(bar).toHaveCount(0);

  // A row's menu starts picking: then a click picks, never opens.
  await row(page, ours[1]).hover();
  await row(page, ours[1])
    .getByRole("button", { name: "Conversation menu" })
    .click();
  await page.getByRole("menuitem", { name: "Select" }).click();
  await expect(bar.getByText("1 selected")).toBeVisible();
  await row(page, ours[0]).locator(".list-row").click();
  await row(page, ours[2]).getByRole("checkbox").check();
  await expect(bar.getByText("3 selected")).toBeVisible();
  await expect(page).not.toHaveURL(/#session=/);

  // Delete: one confirmation naming them, then they are gone.
  await page.getByRole("button", { name: "Delete…" }).click();
  const dialog = page.getByRole("dialog", { name: "Delete 3 conversations?" });
  for (const title of ours) await expect(dialog.getByText(title)).toBeVisible();
  await dialog.getByRole("button", { name: "Delete 3", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  for (const title of ours) await expect(row(page, title)).toHaveCount(0);
  // Nothing left picked: the footer is back.
  await expect(bar).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Settings" })).toBeVisible();
});

test("the Library picks across kinds and deletes them, files and all", async ({
  page,
  host,
  command,
  session,
}) => {
  const design = resolve(host.project, "design/landing");
  await mkdir(design, { recursive: true });
  await writeFile(
    resolve(design, "hero.html"),
    '<!doctype html><meta name="artboard" content="390x844"><title>Hero</title>',
  );
  await mkdir(resolve(host.project, "docs"), { recursive: true });
  await writeFile(
    resolve(host.project, "docs/memo.html"),
    '<!doctype html><meta name="print" content="flow a4"><title>Memo</title>',
  );
  for (const name of ["first-lesson", "second-lesson"])
    await command("memory", {
      cwd: host.project,
      action: "set",
      key: `project/${name}`,
      content: `About ${name}.`,
      revision: "",
    });
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await page
    .getByRole("complementary")
    .getByRole("button", { name: "Library", exact: true })
    .click();
  const rows = page.locator(".library-row:not(.placeholder *)");
  const entry = (name) => rows.filter({ hasText: name });
  await expect(entry("second-lesson")).toBeVisible();

  // ⌘ picks the first; Shift runs to the last, across kinds.
  const names = await rows.locator(".row-label").allInnerTexts();
  expect(names).toHaveLength(4);
  await entry(names[0]).click({ modifiers: ["ControlOrMeta"] });
  await entry(names[3]).click({ modifiers: ["Shift"] });
  const bar = page.getByRole("toolbar", { name: "Selection" });
  await expect(bar.getByText("4 selected")).toBeVisible();
  // The pane beside says what each kind loses.
  const summary = page.getByRole("region", { name: "Selection" });
  await expect(summary).toContainText("2 memories · forgotten");
  await expect(summary).toContainText(
    "1 design · deleted from the project, folder and all",
  );

  await page.keyboard.press("Backspace");
  const dialog = page.getByRole("dialog", {
    name: "Delete 1 design, 1 doc and 2 memories?",
  });
  await dialog.getByRole("button", { name: "Delete 4", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(rows).toHaveCount(0);
  await expect(bar).toHaveCount(0);
  for (const gone of ["design/landing", "docs/memo.html"])
    await expect(access(resolve(host.project, gone))).rejects.toThrow();
  // The place itself stays.
  await access(resolve(host.project, "docs"));
});
