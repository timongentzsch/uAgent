// What needs you, across folders: the sidebar's inbox answers any folder's
// decision, the tab title counts them, a notification's link lands on the
// decision itself, and a coordinator's threads nest under its header.
import { mkdir } from "node:fs/promises";
import { dirname, join } from "node:path";
import { test, expect } from "./fixtures.js";

// A conversation in its own folder that asks to save a memory, in Ask mode.
async function waitingElsewhere(host, command, request) {
  const cwd = join(dirname(host.project), "elsewhere");
  await mkdir(cwd, { recursive: true });
  let { session } = await command("create", { cwd });
  ({ session } = await command("activate", { session_id: session.id }));
  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
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

test("a decision in another folder is answered from the inbox", async ({
  page,
  host,
  session,
  command,
  request,
}) => {
  const other = await waitingElsewhere(host, command, request);
  await page.goto(`/#session=${session.id}`);
  const inbox = page.getByRole("button", { name: "1 needs you" });
  await expect(inbox).toBeVisible();
  // The tab title carries the same count.
  await expect(page).toHaveTitle("(1) µAgent");
  await inbox.click();
  const sheet = page.getByRole("dialog", { name: "Needs you" });
  const waiting = sheet.getByRole("region", {
    name: "Decisions waiting on you",
  });
  // Named with its folder and what it asks.
  await expect(waiting).toContainText("elsewhere");
  if (other.pending_prompt)
    await expect(waiting).toContainText(other.pending_prompt);
  await waiting.getByRole("button", { name: "Allow once" }).click();
  await expect(inbox).toHaveCount(0);
  await expect(page).toHaveTitle("µAgent");
  // The conversation open before stays open.
  expect(new URL(page.url()).hash).toBe(`#session=${session.id}`);
});

test("the inbox opens a waiting session", async ({
  page,
  host,
  session,
  command,
  request,
}) => {
  const other = await waitingElsewhere(host, command, request);
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "1 needs you" }).click();
  const sheet = page.getByRole("dialog", { name: "Needs you" });
  await sheet.locator(".escalation-thread").click();
  await expect(sheet).toHaveCount(0);
  expect(new URL(page.url()).hash).toBe(`#session=${other.id}`);
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
  // The address keeps only the session.
  await expect
    .poll(() => new URL(page.url()).hash)
    .toBe(`#session=${other.id}`);
});

test("a coordinator's threads nest under its folder header", async ({
  page,
  host,
  session,
  command,
}) => {
  await command("create", { cwd: host.project, coordinator: true });
  // Only a coordinator's model starts threads; this one is drawn as one.
  await page.route("**/api/sessions?refresh=1", async (route) => {
    const response = await route.fetch();
    const catalogue = await response.json();
    for (const item of catalogue.sessions)
      if (item.id === session.id)
        Object.assign(item, { kind: "thread", folder: host.project });
    await route.fulfill({ response, json: catalogue });
  });
  await page.goto("/");
  const threads = page.getByRole("group", {
    name: /^Threads of the coordinator for /,
  });
  await expect(threads.locator(".session")).toHaveCount(1);
  // A shape and words beside the colour: nothing waits here, so no icon.
  await expect(threads.locator(".session-state")).toHaveCount(0);
});
