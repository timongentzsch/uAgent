// A space is µAgent's own place for conversations: listed apart from the
// folders, under a name that can be changed, and removable with what is in it.
import { test, expect } from "./fixtures.js";

test("a space is listed apart from folders, renamed and deleted", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "New conversation" }).first().click();
  await page.getByLabel("In a space").fill("Trip to Rome");
  await page.getByRole("button", { name: "Start conversation" }).click();
  const spaces = page.getByRole("group", { name: "Spaces" });
  const folders = page.getByRole("group", { name: "Folders" });
  await expect(spaces.getByRole("heading")).toContainText("Trip to Rome");
  await expect(spaces.locator(".session")).toHaveCount(1);
  await expect(folders.locator(".session")).toHaveCount(1);

  // Renamed, it is the same place with the same conversation in it.
  await spaces
    .getByRole("button", { name: "Menu for the space Trip to Rome" })
    .click();
  await page.getByRole("menuitem", { name: "Rename" }).click();
  await page.getByLabel("Space name").fill("Rome 2027");
  await page.getByRole("button", { name: "Save name" }).click();
  await expect(spaces.getByRole("heading")).toContainText("Rome 2027");
  await expect(spaces.locator(".session")).toHaveCount(1);

  // Deleting says what goes with it, and leaves the folder's alone.
  await spaces
    .getByRole("button", { name: "Menu for the space Rome 2027" })
    .click();
  await page.getByRole("menuitem", { name: "Delete" }).click();
  await expect(page.getByRole("dialog")).toContainText(
    "its 1 conversation and every file in it",
  );
  await page.getByRole("button", { name: "Delete permanently" }).click();
  await expect(spaces).toHaveCount(0);
  await expect(folders.locator(".session")).toHaveCount(1);
  // The conversation that was open went with it.
  await expect(
    page.getByRole("heading", { name: "Your workspace" }),
  ).toBeVisible();
});
