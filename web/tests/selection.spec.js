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

test.describe("touch selection", () => {
  test.use({
    isMobile: true,
    hasTouch: true,
    viewport: { width: 390, height: 844 },
  });

  test("square checks have roomy targets and tapping rows keeps the drawer open", async ({
    page,
    host,
    request,
  }, testInfo) => {
    await three(host, request);
    await page.goto("/");
    await page
      .getByRole("button", { name: "Open conversations", exact: true })
      .tap();
    // A touch row shows no menu button; its menu is still a key away.
    await row(page, "Alpha talk")
      .getByRole("button", { name: "Conversation menu" })
      .press("Enter");
    await page.getByRole("menuitem", { name: "Select", exact: true }).tap();
    const bar = page.getByRole("toolbar", { name: "Selection", exact: true });
    await expect(bar.getByText("1 selected")).toBeVisible();
    await expect(
      page.getByText("Click or tap rows to select", { exact: true }),
    ).toBeVisible();
    const bravo = row(page, "Bravo talk");
    const checkbox = bravo.getByRole("checkbox");
    await expect(checkbox).toHaveCSS("border-radius", "0px");
    await expect(checkbox).toHaveCSS("appearance", "none");
    const target = bravo.locator(".row-check");
    const box = await target.boundingBox();
    expect(box.width).toBeGreaterThanOrEqual(44);
    expect(box.height).toBeGreaterThanOrEqual(44);
    // The input's padding is also tappable, outside the visible square.
    await target.tap({ position: { x: 3, y: 3 } });
    await expect(checkbox).toBeChecked();
    await expect(bar.getByText("2 selected")).toBeVisible();
    await row(page, "Charlie talk").locator(".list-row").tap();
    await expect(bar.getByText("3 selected")).toBeVisible();
    await expect(page).not.toHaveURL(/#session=/);
    await page.screenshot({ path: testInfo.outputPath("phone-selection.png") });
    for (const title of ["Alpha talk", "Bravo talk", "Charlie talk"])
      await row(page, title).locator(".list-row").tap();
    await expect(bar.getByText("0 selected")).toBeVisible();
    await expect(
      page.getByRole("button", { name: "Delete…", exact: true }),
    ).toBeDisabled();
    await bar.getByRole("button", { name: "Done", exact: true }).tap();
    await expect(bar).toHaveCount(0);
    await expect(
      page.getByText("Click or tap rows to select", { exact: true }),
    ).toHaveCount(0);
    await row(page, "Bravo talk").locator(".list-row").tap();
    await expect(page).toHaveURL(/#session=/);
  });
});

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
  await row(page, ours[0])
    .locator(".list-row")
    .click({ modifiers: ["ControlOrMeta"] });
  const bar = page.getByRole("toolbar", { name: "Selection" });
  await expect(bar.getByText("1 selected")).toBeVisible();
  await expect(page).not.toHaveURL(/#session=/);
  await row(page, ours[2])
    .locator(".list-row")
    .click({ modifiers: ["Shift"] });
  await expect(bar.getByText("3 selected")).toBeVisible();
  for (const title of ours)
    await expect(row(page, title).getByRole("checkbox")).toBeChecked();
  // Painting a square must preserve the native keyboard interaction.
  const check = row(page, ours[0]).getByRole("checkbox");
  await check.focus();
  await page.keyboard.press("Space");
  await expect(bar.getByText("2 selected")).toBeVisible();
  await page.keyboard.press("Space");
  await expect(bar.getByText("3 selected")).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(bar).toHaveCount(0);

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

  await page.getByRole("button", { name: "Delete…" }).click();
  const dialog = page.getByRole("dialog", { name: "Delete 3 conversations?" });
  for (const title of ours) await expect(dialog.getByText(title)).toBeVisible();
  await dialog.getByRole("button", { name: "Delete 3", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  for (const title of ours) await expect(row(page, title)).toHaveCount(0);
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

  const names = await rows.locator(".row-label").allInnerTexts();
  expect(names).toHaveLength(4);
  await entry(names[0]).click({ modifiers: ["ControlOrMeta"] });
  await entry(names[3]).click({ modifiers: ["Shift"] });
  const bar = page.getByRole("toolbar", { name: "Selection" });
  await expect(bar.getByText("4 selected")).toBeVisible();
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

test.describe("in Places", () => {
  test.use({ places: true });
  test("a folder's check picks its conversations, half-ticked when only some are", async ({
    page,
    host,
    request,
  }) => {
    await three(host, request);
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.goto("/");
    await page.getByRole("button", { name: "New", exact: true }).click();
    await page.getByRole("menuitem", { name: "Workspace…" }).click();
    await page.getByLabel("Name").fill("Trip");
    await page.getByRole("button", { name: "Open workspace" }).click();
    const folder = page.getByRole("group", { name: "Folders" });
    const rows = folder.locator(".session-row");
    await expect(row(page, "Charlie talk")).toBeVisible();
    const total = await rows.count();
    expect(total).toBeGreaterThanOrEqual(3);
    await row(page, "Alpha talk").hover();
    await row(page, "Alpha talk")
      .getByRole("button", { name: "Conversation menu" })
      .click();
    await page.getByRole("menuitem", { name: "Select" }).click();
    const bar = page.getByRole("toolbar", { name: "Selection" });
    const check = folder.getByRole("checkbox", {
      name: /^Select the conversations of /,
    });
    await expect(check).toBeChecked({ indeterminate: true });
    await check.click();
    await expect(bar.getByText(`${total} selected`)).toBeVisible();
    await expect(rows.getByRole("checkbox", { checked: true })).toHaveCount(
      total,
    );
    await row(page, "Bravo talk").getByRole("checkbox").uncheck();
    await expect(check).toBeChecked({ indeterminate: true });
    await check.click();
    await expect(check).toBeChecked();
    await expect(bar.getByText(`${total} selected`)).toBeVisible();
    await check.click();
    await expect(bar.getByText("0 selected")).toBeVisible();

    // Folded, its rows are not shown, yet the check still picks them.
    await folder.getByRole("button", { name: /^Conversations of / }).click();
    await check.click();
    await expect(bar.getByText(`${total} selected`)).toBeVisible();

    // A workspace is a thing of its own: its check picks the workspace.
    await page
      .getByRole("group", { name: "Workspaces" })
      .getByRole("checkbox", { name: "Select the workspace Trip" })
      .click();
    await expect(bar.getByText(`${total + 1} selected`)).toBeVisible();
  });
});
