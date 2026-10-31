// A pinned conversation or folder moves to the top of the sidebar. The pins
// belong to the host: every paired device lists the same.
import { test, expect, online, withoutServiceWorker } from "./fixtures.js";

const lists = (page) => {
  const nav = page.getByRole("navigation", { name: "Conversations" });
  return {
    pinned: nav.getByRole("group", { name: "Pinned" }),
    folders: nav.getByRole("group", { name: "Folders" }),
  };
};

test("a conversation is pinned from its menu and unpinned again", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const { pinned, folders } = lists(page);
  await expect(folders.locator(".session")).toHaveCount(1);
  await expect(pinned).toHaveCount(0);

  await folders.getByRole("button", { name: "Conversation menu" }).click();
  await page.getByRole("menuitem", { name: "Pin", exact: true }).click();
  await expect(pinned.locator(".session")).toHaveCount(1);
  // It leaves its place rather than showing twice.
  await expect(folders.locator(".session")).toHaveCount(0);

  await pinned.getByRole("button", { name: "Conversation menu" }).click();
  await page.getByRole("menuitem", { name: "Unpin", exact: true }).click();
  await expect(pinned).toHaveCount(0);
  await expect(folders.locator(".session")).toHaveCount(1);
});

test("a plain folder is pinned from its menu, with its conversations", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const { pinned, folders } = lists(page);
  await folders.getByRole("button", { name: /^Folder menu for / }).click();
  await page.getByRole("menuitem", { name: "Pin", exact: true }).click();
  await expect(pinned.getByRole("heading")).toHaveCount(1);
  await expect(pinned.locator(".session")).toHaveCount(1);
  await expect(folders).toHaveCount(0);

  await pinned.getByRole("button", { name: /^Folder menu for / }).click();
  await page.getByRole("menuitem", { name: "Unpin", exact: true }).click();
  await expect(pinned).toHaveCount(0);
  await expect(folders.locator(".session")).toHaveCount(1);
});

test("another browser lists the same pins live", async ({
  browser,
  page,
  session,
  storageState,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const other = await (await browser.newContext({ storageState })).newPage();
  await withoutServiceWorker(other);
  await other.setViewportSize({ width: 1440, height: 900 });
  await other.goto(`/#session=${session.id}`);
  await expect(lists(other).folders.locator(".session")).toHaveCount(1);

  await lists(page)
    .folders.getByRole("button", { name: "Conversation menu" })
    .click();
  await page.getByRole("menuitem", { name: "Pin", exact: true }).click();
  await expect(lists(other).pinned.locator(".session")).toHaveCount(1);

  await lists(page)
    .pinned.getByRole("button", { name: "Conversation menu" })
    .click();
  await page.getByRole("menuitem", { name: "Unpin", exact: true }).click();
  await expect(lists(other).pinned).toHaveCount(0);
});

test("deleting a pinned conversation removes it from Pinned", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const { pinned } = lists(page);
  await lists(page)
    .folders.getByRole("button", { name: "Conversation menu" })
    .click();
  await page.getByRole("menuitem", { name: "Pin", exact: true }).click();
  await expect(pinned.locator(".session")).toHaveCount(1);

  await pinned.getByRole("button", { name: "Conversation menu" }).click();
  await page.getByRole("menuitem", { name: "Delete" }).click();
  await page.getByRole("button", { name: "Delete permanently" }).click();
  await expect(pinned).toHaveCount(0);
});

test("Pin is the first item of a menu and works from the keyboard", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const { pinned, folders } = lists(page);
  await folders.getByRole("button", { name: /^Folder menu for / }).focus();
  await page.keyboard.press("Enter");
  const pin = page.getByRole("menuitem", { name: "Pin", exact: true });
  await expect(pin).toBeFocused();
  await page.keyboard.press("Enter");
  await expect(pinned.getByRole("heading")).toHaveCount(1);

  await pinned.getByRole("button", { name: "Conversation menu" }).focus();
  await page.keyboard.press("Enter");
  await expect(pin).toBeFocused();
  await page.keyboard.press("Enter");
  await expect(pinned.locator(".session")).toHaveCount(1);
});
