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

test("on a phone the home screen lists the latest above a docked start box, with long titles cut and room to type", async ({
  page,
  host,
  request,
}) => {
  for (let at = 1; at <= 9; at++)
    await unfiled(
      host,
      request,
      `Conversation ${at} with a title that is far longer than any phone is wide, by a good margin`,
    );
  await page.setViewportSize({ width: 390, height: 700 });
  await page.goto("/");
  const waiting = page.getByRole("region", { name: "Recent" });
  await expect(waiting.getByRole("listitem")).toHaveCount(6);
  // Nothing runs off the side, and the field keeps most of the width.
  expect(
    await page.evaluate(() => document.documentElement.scrollWidth),
  ).toBeLessThanOrEqual(390);
  // Every line of a row ends inside it, cut with an ellipsis where it is
  // longer: measured on the text, which a clipped row would still hide.
  const overruns = await waiting.getByRole("listitem").evaluateAll((rows) =>
    rows.flatMap((row) => {
      const edge = row.getBoundingClientRect().right;
      return [...row.querySelectorAll(".truncate")]
        .filter(
          (line) =>
            line.getBoundingClientRect().right > edge + 0.5 ||
            (line.scrollWidth > line.clientWidth &&
              getComputedStyle(line).textOverflow !== "ellipsis"),
        )
        .map((line) => line.textContent);
    }),
  );
  expect(overruns).toEqual([]);
  expect(
    await waiting
      .locator(".truncate")
      .first()
      .evaluate((line) => line.scrollWidth > line.clientWidth),
  ).toBe(true);
  // The start box is docked at the bottom, as wide as the screen allows,
  // and a list too long for the room above it scrolls clear of it.
  await page.setViewportSize({ width: 390, height: 420 });
  const start = page.locator(".home-start");
  const field = await page.getByLabel("Start a conversation").boundingBox();
  expect(field.width).toBeGreaterThan(300);
  await expect
    .poll(async () => {
      const docked = await start.boundingBox();
      return docked.y > 210 && docked.y + docked.height <= 420;
    })
    .toBe(true);
  const last = waiting.getByRole("listitem").last();
  const under = async () =>
    (await last.boundingBox()).y +
    (await last.boundingBox()).height -
    (await start.boundingBox()).y;
  expect(await under()).toBeGreaterThan(0);
  await page.locator(".home").evaluate((home) => {
    home.scrollTop = home.scrollHeight;
  });
  await expect.poll(under).toBeLessThanOrEqual(1);
});

test("the start box is the composer's own box, and the list leads back to it from a conversation", async ({
  page,
  host,
  request,
}) => {
  const session = await savedConversation(host, request, "Somewhere else");
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const look = (form) =>
    form.evaluate((node) => {
      const style = getComputedStyle(node);
      return [style.border, style.backgroundColor];
    });
  const composer = await look(page.locator(".composer > form"));
  // The two pages sit under the list, over the connection line.
  const pages = page.locator(".sidebar-pages button");
  await expect(pages).toHaveText(["Library", "Scheduled"]);
  const edge = (selector, side) =>
    page
      .locator(selector)
      .first()
      .evaluate((node, side) => node.getBoundingClientRect()[side], side);
  expect(await edge(".sidebar-pages", "top")).toBeGreaterThanOrEqual(
    await edge(".sidebar nav", "bottom"),
  );
  expect(await edge(".sidebar-pages", "bottom")).toBeLessThanOrEqual(
    await edge(".sidebar footer", "top"),
  );
  await page
    .locator(".sidebar")
    .getByRole("button", { name: "Home", exact: true })
    .click();
  const start = page.getByLabel("Start a conversation");
  await expect(start).toBeVisible();
  expect(await look(page.locator(".start-field"))).toEqual(composer);
});

test("on a phone the list's sheet leads home, and search is a panel with the screen round it", async ({
  page,
  host,
  request,
}) => {
  const session = await savedConversation(host, request, "Somewhere else");
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Open conversations" }).click();
  const sheet = page.getByRole("dialog", { name: "Conversations" });
  await sheet.getByRole("button", { name: "Search", exact: true }).click();
  const search = page.getByRole("dialog", { name: "Search" });
  await expect(search).toBeVisible();
  const box = await search.boundingBox();
  expect(box.x).toBeGreaterThan(0);
  expect(box.y).toBeGreaterThan(0);
  expect(box.x + box.width).toBeLessThan(390);
  expect(Math.abs(box.x - (390 - box.x - box.width))).toBeLessThanOrEqual(1);
  await page.keyboard.press("Escape");
  // Search took the sheet's place; opened again, it leads home.
  await page.getByRole("button", { name: "Open conversations" }).click();
  await sheet.getByRole("button", { name: "Home", exact: true }).click();
  await expect(page.getByLabel("Start a conversation")).toBeVisible();
});

test("a model chosen on the home screen is the one its conversation starts with", async ({
  page,
}) => {
  await page.goto("/");
  await expect(page.getByLabel("Start a conversation")).toBeEnabled();
  const choice = page.getByRole("button", {
    name: "Model for this conversation",
    exact: true,
  });
  await expect(choice).toHaveText("Default model");
  await choice.click();
  await page.getByLabel("Model", { exact: true }).fill("model-b");
  await page.getByRole("option").filter({ hasText: "mock/model-b" }).click();
  await page.getByRole("button", { name: "Apply", exact: true }).click();
  await expect(choice).toHaveText("mock/model-b");
  await page.getByLabel("Start a conversation").fill("Model probe");
  await page.getByRole("button", { name: "Start", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "Model and effort", exact: true }),
  ).toContainText("mock/model-b");
});

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
  // Home lists what has a place suggested, and says nothing of the rest.
  const waiting = page.getByRole("region", { name: "Suggested moves" });
  await expect(waiting.getByRole("listitem")).toHaveCount(1);
  await expect(waiting).toContainText("It changed 2 files there.");
  await expect(page.getByText("No place suggested")).toHaveCount(0);
  // The whole list is beside it at this width, so no second one repeats it.
  await expect(page.getByRole("region", { name: "Recent" })).toHaveCount(0);
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
