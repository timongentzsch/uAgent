// What needs you, across folders: the sidebar's Needs you, the tab title's
// count, a notification's link to the decision, and a conversation's agents.
import { mkdir } from "node:fs/promises";
import { dirname, join } from "node:path";
import { test, expect, selectProbeModel } from "./fixtures.js";

// A conversation in its own folder that asks to save a memory, in Ask mode.
async function waitingElsewhere(host, command, request) {
  const cwd = join(dirname(host.project), "elsewhere");
  await mkdir(cwd, { recursive: true });
  let { session } = await command("create", { cwd });
  ({ session } = await command("activate", { session_id: session.id }));
  await selectProbeModel(command, session);
  await command("submit", {
    session_id: session.id,
    generation: session.generation,
    text: "Memory receipt probe: please save this test memory",
  });
  await expect
    .poll(async () => {
      const response = await request.get(`/api/sessions/${session.id}`);
      session = (await response.json()).metadata;
      return session.pending;
    })
    .toBe(true);
  return session;
}

test("a decision in another folder waits under Needs you, listed once in either view", async ({
  page,
  host,
  session,
  command,
  request,
}) => {
  const other = await waitingElsewhere(host, command, request);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const nav = page.getByRole("navigation", { name: "Conversations" });
  const needs = nav.getByRole("group", { name: "Needs you" });
  await expect(page).toHaveTitle("(1) µAgent");
  for (const view of ["Recent", "Places"]) {
    await page.getByRole("button", { name: view, exact: true }).click();
    await expect(needs.locator(".session")).toHaveCount(1);
    await expect(
      needs.getByRole("img", { name: "Needs your input" }),
    ).toBeVisible();
    await expect(nav.locator(".session")).toHaveCount(2);
  }
  // Its folder holds nothing else, so Places has no place for it.
  await expect(
    nav.getByRole("button", { name: /^Conversations of .*elsewhere$/ }),
  ).toHaveCount(0);

  // Pinned, it is listed among the pinned alone.
  const pinned = nav.getByRole("group", { name: "Pinned" });
  for (const [from, item] of [
    [needs, "Pin"],
    [pinned, "Unpin"],
  ]) {
    await from.locator(".session-row").hover();
    await from.getByRole("button", { name: "Conversation menu" }).click();
    await page.getByRole("menuitem", { name: item, exact: true }).click();
    await expect(from).toHaveCount(0);
    await expect(nav.locator(".session")).toHaveCount(2);
  }

  await needs.locator(".list-row").click();
  await expect(page).toHaveURL(new RegExp(`#session=${other.id}$`));
  await page.getByRole("button", { name: "Allow once" }).click();
  await expect(needs).toHaveCount(0);
  await expect(page).toHaveTitle("µAgent");
  await expect(
    nav.getByRole("button", { name: /^Conversations of .*elsewhere$/ }),
  ).toHaveCount(1);
});

test("a notification's link opens the session at its decision", async ({
  page,
  host,
  command,
  request,
}) => {
  const other = await waitingElsewhere(host, command, request);
  await page.goto(`/#session=${other.id}&decision=attention-1`);
  await expect(page.locator(".conversation .decision h2")).toBeFocused();
  await expect
    .poll(() => new URL(page.url()).hash)
    .toBe(`#session=${other.id}`);
});

test("a conversation's agents at work sit beneath it, finished ones leave the list", async ({
  page,
  host,
  session,
  command,
}) => {
  const { session: other } = await command("create", { cwd: host.project });
  const { session: done } = await command("create", { cwd: host.project });
  // Only a model starts agents; these two are drawn as the conversation's,
  // one at work and one finished. Each has a turn, or the list leaves it out.
  let listed = 0;
  await page.route("**/api/sessions?refresh=1", async (route) => {
    const response = await route.fetch();
    const catalogue = await response.json();
    listed = catalogue.sessions.length;
    for (const item of catalogue.sessions) item.turns = 1;
    for (const item of catalogue.sessions)
      if (item.id === other.id || item.id === done.id) {
        item.parent = session.id;
        item.turn_active = item.id === other.id;
      }
    await route.fulfill({ response, json: catalogue });
  });
  await page.goto("/");
  const agents = page.getByRole("group", { name: /^Agents of / });
  await expect(agents.locator(".session")).toHaveCount(1);
  await expect(agents.locator(".session-state .status-led")).toHaveCount(1);
  // The finished one has no row, and nothing folds it away.
  await expect(page.getByRole("button", { name: /agent$/ })).toHaveCount(0);
  await expect(page.locator(".sidebar .session")).toHaveCount(listed - 1);
});
