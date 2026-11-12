// A workspace is µAgent's own place for conversations, listed apart from the
// folders. A place is a header over its rows, which fold and are remembered.
import { test, expect, online, savedConversation } from "./fixtures.js";

test("a workspace is listed apart from folders, renamed and deleted", async ({
  page,
  host,
  request,
}) => {
  const session = await savedConversation(host, request, "In the folder");
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "New", exact: true }).click();
  await page.getByRole("menuitem", { name: "Workspace…" }).click();
  await page.getByLabel("Name").fill("Trip to Rome");
  await page.getByRole("button", { name: "Open workspace" }).click();
  const workspaces = page.getByRole("group", { name: "Workspaces" });
  const folders = page.getByRole("group", { name: "Folders" });
  await expect(workspaces.getByRole("heading")).toContainText("Trip to Rome");
  // Opening a workspace starts a conversation in it.
  await expect(workspaces.locator(".session")).toHaveCount(1);
  await expect(folders.locator(".session")).toHaveCount(1);

  await workspaces
    .getByRole("button", { name: "Menu for the workspace Trip to Rome" })
    .click();
  await page.getByRole("menuitem", { name: "Rename" }).click();
  await page.getByLabel("Workspace name").fill("Rome 2027");
  await page.getByRole("button", { name: "Save name" }).click();
  await expect(workspaces.getByRole("heading")).toContainText("Rome 2027");
  await expect(workspaces.locator(".session")).toHaveCount(1);

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
  await expect(page.getByRole("heading", { name: "µAgent" })).toBeVisible();
});

test("a place folds to its header with a count, remembered, and + starts a conversation in it", async ({
  page,
  host,
  request,
}) => {
  const session = await savedConversation(host, request, "In the folder");
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const folders = page.getByRole("group", { name: "Folders" });
  const header = folders.getByRole("button", {
    name: /^Conversations of /,
    expanded: true,
  });
  await expect(folders.locator(".session")).toHaveCount(1);
  await expect(folders.locator(".place-count")).toHaveText("1");
  // Its own name; no other listed folder shares it, so no parent.
  const path = host.project.split("/").filter(Boolean);
  await expect(folders.locator(".folder-own")).toHaveText(path.at(-1));
  await expect(folders.locator(".folder-parent")).toHaveCount(0);

  await header.click();
  await expect(folders.locator(".session")).toHaveCount(0);
  await expect(folders.locator(".place-count")).toHaveText("1");
  await expect(
    folders.getByRole("button", {
      name: /^Conversations of /,
      expanded: false,
    }),
  ).toHaveAttribute("aria-current", "true");
  await page.reload();
  await expect(folders.locator(".session")).toHaveCount(0);
  // Alt+↓ steps through the rows shown, and none is.
  const at = page.url();
  await page.keyboard.press("Alt+ArrowDown");
  expect(page.url()).toBe(at);

  await folders.getByRole("button", { name: /^New in / }).click();
  await page.getByRole("menuitem", { name: "Conversation" }).click();
  await expect(folders.locator(".session")).toHaveCount(2);
  // Each place counts its own (macOS lists /var and /private/var apart).
  await expect
    .poll(async () =>
      (await folders.locator(".place-count").allInnerTexts()).reduce(
        (sum, count) => sum + Number(count),
        0,
      ),
    )
    .toBe(2);
});

test("New conversation starts in the Scratch workspace, and a second joins it", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const workspaces = page.getByRole("group", { name: "Workspaces" });
  for (let started = 0; started < 2; started++) {
    const before = page.url();
    await page.getByRole("button", { name: "New", exact: true }).click();
    await page.getByRole("menuitem", { name: "Conversation" }).click();
    // The one it left, still empty, is left out of the list.
    await expect(workspaces.locator(".session")).toHaveCount(1);
    // The page has gone to it: typing before this reaches the one it left.
    await expect(page).not.toHaveURL(before);
    await expect(
      workspaces.locator('.session[aria-current="page"]'),
    ).toHaveCount(1);
    await expect(workspaces.getByRole("heading")).toHaveCount(1);
    await expect(workspaces.getByRole("heading")).toContainText("Scratch");
    await expect(page.locator(".composer .activity-button")).toHaveCount(0);
  }
  await online(page);
  await page.locator(".composer textarea").fill("hello");
  await expect(page.locator(".composer textarea")).toHaveValue("hello");
});
