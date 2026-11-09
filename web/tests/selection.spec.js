// Several conversations picked at once in the sidebar, as a desktop file
// list picks them, and acted on together: the multi-select canvas.
import { test, expect, savedConversation } from "./fixtures.js";

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
