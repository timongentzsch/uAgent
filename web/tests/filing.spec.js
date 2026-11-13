// A conversation starts in Scratch and is filed later, where its host
// suggests: from the home screen, the list, or the conversation itself.
import { mkdir, realpath } from "node:fs/promises";
import {
  test,
  expect,
  online,
  saveHistory,
  savedConversation,
} from "./fixtures.js";

test.use({ places: true });

// A conversation saved in Scratch with the host's suggestion for it.
async function unfiled(host, request, title, suggestion) {
  const scratch = `${host.home}/.uagent/spaces/Scratch`;
  await mkdir(`${scratch}/.uagent`, { recursive: true });
  const id = crypto.randomUUID();
  await saveHistory(host, `saved-${id}.json`, {
    id,
    title,
    turns: 1,
    cwd: await realpath(scratch),
    suggestion,
    messages: [
      { role: "user", content: title },
      { role: "assistant", content: "Done" },
    ],
  });
  let found;
  await expect
    .poll(async () => {
      const response = await request.get("/api/sessions?refresh=1");
      const { sessions } = await response.json();
      found = sessions.find((item) => item.title === title);
      return !!found;
    })
    .toBe(true);
  return found;
}

test("what waits in Scratch is filed from the home screen", async ({
  page,
  host,
  request,
}) => {
  await unfiled(host, request, "Fix the flaky test", {
    options: [{ cwd: host.project }],
    reason: "It changed 2 files there.",
  });
  await unfiled(host, request, "What does SIGPIPE do");
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/");
  await expect(page.getByLabel("Start a conversation")).toBeEnabled();
  const waiting = page.getByRole("region", { name: "To file" });
  await expect(waiting.getByRole("listitem")).toHaveCount(2);
  await expect(waiting).toContainText("It changed 2 files there.");
  await expect(waiting).toContainText("No place suggested");
  // The list names only what has a suggestion.
  const listed = page.getByRole("group", { name: "To file" });
  await expect(listed.locator(".session")).toHaveCount(1);
  await expect(listed).toContainText("Fix the flaky test");

  // What Move does is the test below this file's last; here the row has it.
  await expect(
    waiting.getByRole("button", { name: /^Move to / }),
  ).toBeEnabled();
});

test("a suggestion is answered in the conversation, and stays answered", async ({
  page,
  host,
  request,
}) => {
  const session = await unfiled(host, request, "Three days in Lisbon", {
    options: [{ workspace: "Lisbon trip" }],
    reason: "No place you have fits it.",
  });
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const bar = page.getByRole("region", { name: "Suggested place" });
  await expect(bar).toContainText("A new workspace, “Lisbon trip”");
  await expect(
    bar.getByRole("button", { name: "Create and move" }),
  ).toBeEnabled();
  await bar.getByRole("button", { name: "Keep in Scratch" }).click();
  await expect(bar).toBeHidden();
  await expect
    .poll(async () => {
      const response = await request.get("/api/sessions?refresh=1");
      const { sessions } = await response.json();
      return sessions.find((item) => item.id === session.id)?.suggestion;
    })
    .toEqual({ dismissed: true });
  await page.reload();
  await online(page);
  await expect(page.locator(".message.response")).toContainText("Done");
  await expect(bar).toBeHidden();
});

test.describe("with a model that places", () => {
  test.use({ titles: true });
  test("a finished conversation is offered its place, and Move takes it there", async ({
    page,
    host,
    request,
  }) => {
    await savedConversation(host, request, "Pairing over SSH");
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.goto("/");
    const field = page.getByLabel("Start a conversation");
    await expect(field).toBeEnabled();
    await field.fill("File me probe");
    await field.press("Enter");
    await expect(page.locator(".message.response")).toContainText(
      "Verified response",
    );
    // Asked once the turn had ended, the model names the folder.
    const bar = page.getByRole("region", { name: "Suggested place" });
    await expect(bar).toContainText("This belongs in");
    const listed = page.getByRole("group", { name: "To file" });
    await expect(listed.locator(".session")).toHaveCount(1);
    const before = page.url();
    await bar.getByRole("button", { name: /^Move to / }).click();
    // The page follows the conversation to its new address, in the folder.
    await expect(page).not.toHaveURL(before);
    await expect(bar).toBeHidden();
    await expect(listed).toBeHidden();
    await expect(
      page.getByRole("group", { name: "Folders" }).locator(".session"),
    ).toHaveCount(2);
    await expect(page.locator(".message.response")).toContainText(
      "Verified response",
    );
  });

  test("a first message joins the conversation it continues, before anything has changed", async ({
    page,
    host,
    request,
  }) => {
    const earlier = await savedConversation(host, request, "Pairing over SSH");
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.goto("/");
    const field = page.getByLabel("Start a conversation");
    await expect(field).toBeEnabled();
    // The scripted provider looks around once, then answers slowly.
    await field.fill("Filing probe");
    await field.press("Enter");
    const bar = page.getByRole("region", { name: "Suggested place" });
    await expect(bar).toContainText("Continue “Pairing over SSH”");
    await expect(bar).toContainText("Nothing has changed yet");
    await bar.getByRole("button", { name: "Continue there" }).click();
    await expect
      .poll(() => new URL(page.url()).hash)
      .toBe(`#session=${earlier.id}`);
    await expect(page.locator(".message.user").last()).toContainText(
      "Filing probe",
    );
    // The conversation it left had done nothing, and is gone.
    await expect(
      page.getByRole("group", { name: "Workspaces" }).locator(".session"),
    ).toHaveCount(0);
  });
});
