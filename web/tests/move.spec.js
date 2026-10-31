// A conversation moves, with its agents, to another folder. Its id changes
// with its place, so every open page follows it to the new one.
import { mkdir } from "node:fs/promises";
import {
  test,
  expect,
  online,
  selectProbeModel,
  withoutServiceWorker,
} from "./fixtures.js";

test.beforeEach(({ command, session }) => selectProbeModel(command, session));

const idOf = (page) =>
  new URLSearchParams(new URL(page.url()).hash.slice(1)).get("session");

// Sends a first message, so the conversation is saved and can move.
async function say(page, session, text = "Hello") {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.getByLabel("Message or guidance").fill(text);
  await page.getByRole("button", { name: "Send", exact: true }).click();
}

async function openMove(page) {
  await page
    .getByRole("navigation", { name: "Conversations" })
    .getByRole("button", { name: "Conversation menu" })
    .click();
  await page.getByRole("menuitem", { name: "Move to…" }).click();
  return page.getByRole("dialog", { name: /^Move “/ });
}

test("a conversation moves to a typed folder and the page follows it", async ({
  page,
  browser,
  session,
  storageState,
}, testInfo) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await say(page, session);
  await expect(page.locator(".message.response")).toContainText(
    "Verified response",
  );
  const other = await (await browser.newContext({ storageState })).newPage();
  await withoutServiceWorker(other);
  await other.setViewportSize({ width: 1440, height: 900 });
  await other.goto(`/#session=${session.id}`);
  await online(other);

  const destination = testInfo.outputPath("elsewhere");
  await mkdir(destination, { recursive: true });
  // Words not yet sent stay with the conversation.
  await page.getByLabel("Message or guidance").fill("Words not yet sent");
  const dialog = await openMove(page);
  await expect(dialog).toContainText("Moves the conversation. Files stay in");
  // Where it is now is listed, and cannot be chosen.
  await expect(dialog.getByRole("button", { name: /Current/ })).toBeDisabled();
  const move = dialog.getByRole("button", { name: "Move", exact: true });
  await expect(move).toBeDisabled();
  await dialog.getByLabel("Place").fill(destination);
  await move.click();

  const folders = page.getByRole("group", { name: "Folders" });
  await expect(folders.locator(".folder-own")).toHaveText("elsewhere");
  await expect(folders.locator(".session")).toHaveCount(1);
  await expect.poll(() => idOf(page)).not.toBe(session.id);
  // Still the open conversation, with what it said.
  await expect(page.locator(".message.response")).toContainText(
    "Verified response",
  );
  await expect(page.locator(".transcript")).toContainText("Moved");
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "Words not yet sent",
  );
  // Another device follows it to the same address.
  await expect.poll(() => idOf(other)).toBe(idOf(page));
  await expect(other.locator(".message.response")).toContainText(
    "Verified response",
  );
});

test("a running turn keeps the conversation where it is, and the dialog says so", async ({
  page,
  command,
  session,
}, testInfo) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await say(page, session, "Queue controls probe");
  await expect(page.locator(".message.response")).toContainText(
    "Waiting for guidance",
  );
  const destination = testInfo.outputPath("later");
  await mkdir(destination, { recursive: true });
  const dialog = await openMove(page);
  await dialog.getByLabel("Place").fill(destination);
  await expect(dialog).toContainText("cannot move while");
  await expect(dialog).toContainText("running");
  const move = dialog.getByRole("button", { name: "Move", exact: true });
  await expect(move).toBeDisabled();
  // Stopped, the dialog follows without being opened again.
  await command("interrupt", {
    session_id: session.id,
    generation: session.generation,
  });
  await expect(dialog).not.toContainText("cannot move while");
  await expect(move).toBeEnabled();
});

test("the menu offers no move before a conversation is saved", async ({
  page,
  command,
  host,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  const { session: draft } = await command("create", { cwd: host.project });
  await page.goto(`/#session=${draft.id}`);
  await online(page);
  await page
    .locator("header.conversation-head")
    .getByRole("button", { name: "Conversation menu" })
    .click();
  await expect(page.getByRole("menuitem", { name: "Rename" })).toBeVisible();
  await expect(page.getByRole("menuitem", { name: "Move to…" })).toHaveCount(0);
});
