// A workspace is µAgent's own place for conversations: listed apart from the
// folders, under a name that can be changed, and removable with what is in it.
// A place is a header over its rows, which fold away and are remembered.
import { test, expect } from "./fixtures.js";

test("a workspace is listed apart from folders, renamed and deleted", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "New", exact: true }).click();
  await page.getByRole("menuitem", { name: "Workspace…" }).click();
  await page.getByLabel("Name").fill("Trip to Rome");
  await page.getByRole("button", { name: "Start conversation" }).click();
  const workspaces = page.getByRole("group", { name: "Workspaces" });
  const folders = page.getByRole("group", { name: "Folders" });
  await expect(workspaces.getByRole("heading")).toContainText("Trip to Rome");
  await expect(workspaces.locator(".session")).toHaveCount(1);
  await expect(folders.locator(".session")).toHaveCount(1);

  // Renamed, it is the same place with the same conversation in it.
  await workspaces
    .getByRole("button", { name: "Menu for the workspace Trip to Rome" })
    .click();
  await page.getByRole("menuitem", { name: "Rename" }).click();
  await page.getByLabel("Workspace name").fill("Rome 2027");
  await page.getByRole("button", { name: "Save name" }).click();
  await expect(workspaces.getByRole("heading")).toContainText("Rome 2027");
  await expect(workspaces.locator(".session")).toHaveCount(1);

  // Deleting says what goes with it, and leaves the folder's alone.
  await workspaces
    .getByRole("button", { name: "Menu for the workspace Rome 2027" })
    .click();
  await page.getByRole("menuitem", { name: "Delete" }).click();
  await expect(page.getByRole("dialog")).toContainText(
    "its 1 conversation and every file in it",
  );
  await page.getByRole("button", { name: "Delete permanently" }).click();
  await expect(workspaces).toHaveCount(0);
  await expect(folders.locator(".session")).toHaveCount(1);
  // The conversation that was open went with it.
  await expect(
    page.getByRole("heading", { name: "Your workspace" }),
  ).toBeVisible();
});

test("a place folds to its header with a count, remembered, and + starts a conversation in it", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const folders = page.getByRole("group", { name: "Folders" });
  const header = folders.getByRole("button", {
    name: /^Conversations of /,
    expanded: true,
  });
  await expect(folders.locator(".session")).toHaveCount(1);

  await header.click();
  await expect(folders.locator(".session")).toHaveCount(0);
  await expect(folders.locator(".place-count")).toHaveText("(1)");
  // The open conversation is inside: the header is marked as its row was.
  await expect(
    folders.getByRole("button", {
      name: /^Conversations of /,
      expanded: false,
    }),
  ).toHaveAttribute("aria-current", "true");
  // The fold is remembered.
  await page.reload();
  await expect(folders.locator(".session")).toHaveCount(0);
  // Alt+↓ steps through the rows shown, and none is.
  const at = page.url();
  await page.keyboard.press("Alt+ArrowDown");
  expect(page.url()).toBe(at);

  // + opens the fold, and starts a conversation in the place.
  await folders.getByRole("button", { name: /^New conversation in / }).click();
  await expect(folders.locator(".session")).toHaveCount(2);
  await expect(folders.locator(".place-count")).toHaveCount(0);
});
