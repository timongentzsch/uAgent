// The list as Recent shows it, by day across places, or as Places, each place
// with its last week; the choice is the device's.
import { test, expect, savedConversation } from "./fixtures.js";

const DAY = 24 * 60 * 60 * 1000;

// Saved conversations, listed as changed the given number of days ago.
async function aged(page, host, request, ages) {
  const saved = {};
  for (const title of Object.keys(ages))
    saved[title] = await savedConversation(host, request, title);
  await page.route(/\/api\/sessions(?:\?.*)?$/, async (route) => {
    const response = await route.fetch();
    const catalogue = await response.json();
    for (const item of catalogue.sessions)
      if (item.title in ages)
        item.updated = Date.now() - ages[item.title] * DAY;
    await route.fulfill({ response, json: catalogue });
  });
  return saved;
}

const AGES = { Fresh: 0, "Last night": 1, Monday: 3, "Long ago": 30 };

test("Recent groups by day with Earlier folded, and Places is kept once chosen", async ({
  page,
  host,
  request,
}) => {
  const saved = await aged(page, host, request, AGES);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/");
  const nav = page.getByRole("navigation", { name: "Conversations" });
  const day = (name) => nav.getByRole("group", { name, exact: true });
  const recent = page.getByRole("button", { name: "Recent", exact: true });
  const places = page.getByRole("button", { name: "Places", exact: true });
  await expect(recent).toHaveAttribute("aria-pressed", "true");
  await expect(day("Today").locator(".row-label")).toHaveText(["Fresh"]);
  await expect(day("Yesterday").locator(".row-label")).toHaveText([
    "Last night",
  ]);
  await expect(day("This week").locator(".row-label")).toHaveText(["Monday"]);
  // Each row names its place, here the folder's own name.
  await expect(day("Today").locator(".session-tag")).toHaveText(
    host.project.split("/").at(-1),
  );
  const earlier = day("Earlier").getByRole("button", { name: "Earlier" });
  await expect(earlier).toHaveAttribute("aria-expanded", "false");
  await expect(day("Earlier").locator(".session")).toHaveCount(0);
  await earlier.click();
  await expect(day("Earlier").locator(".row-label")).toHaveText(["Long ago"]);
  await expect(nav.getByRole("group", { name: "Folders" })).toHaveCount(0);

  await places.click();
  await page.reload();
  await expect(places).toHaveAttribute("aria-pressed", "true");
  const folders = nav.getByRole("group", { name: "Folders" });
  await expect(day("Today")).toHaveCount(0);
  // The last week, and at least the three newest; the rest behind one row.
  const older = folders.getByRole("button", { name: "1 older" });
  await expect(folders.locator(".row-label")).toHaveText([
    "Fresh",
    "Last night",
    "Monday",
  ]);
  await expect(folders.locator(".session-time")).toHaveCount(3);
  await expect(older).toHaveAttribute("aria-expanded", "false");
  // The open conversation is listed however old it is.
  await page.goto(`/#session=${saved["Long ago"].id}`);
  await expect(folders.locator(".session")).toHaveCount(4);
  await expect(older).toHaveCount(0);
  await page.goto("/");
  await older.click();
  await expect(folders.locator(".row-label")).toHaveText([
    "Fresh",
    "Last night",
    "Monday",
    "Long ago",
  ]);
  await page.reload();
  await expect(older).toHaveAttribute("aria-expanded", "true");
  await expect(folders.locator(".session")).toHaveCount(4);
});

test("a Shift run in Recent spans its day groups, and ⌘A takes the rows shown", async ({
  page,
  host,
  request,
}) => {
  await aged(page, host, request, AGES);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/");
  const nav = page.getByRole("navigation", { name: "Conversations" });
  const row = (title) =>
    nav.locator(".session-row", {
      has: page.getByText(title, { exact: true }),
    });
  await row("Fresh")
    .locator(".list-row")
    .click({ modifiers: ["ControlOrMeta"] });
  await row("Monday")
    .locator(".list-row")
    .click({ modifiers: ["Shift"] });
  const bar = page.getByRole("toolbar", { name: "Selection" });
  await expect(bar.getByText("3 selected")).toBeVisible();
  for (const title of ["Fresh", "Last night", "Monday"])
    await expect(row(title).getByRole("checkbox")).toBeChecked();
  // Earlier is folded: its row is not one of those shown.
  await page.keyboard.press("ControlOrMeta+a");
  await expect(bar.getByText("3 selected")).toBeVisible();
});
