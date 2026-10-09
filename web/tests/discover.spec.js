// Finding your way without knowing where things are: the command palette,
// the shortcuts sheet, conversations by keyboard, the home screen, and pairing
// by link.
import { test, expect } from "./fixtures.js";

const mod = process.platform === "darwin" ? "Meta" : "Control";

test("the palette opens a conversation and runs a command", async ({
  page,
  session,
  command,
}) => {
  await command("rename", {
    session_id: session.id,
    generation: session.generation,
    title: "Palette probe",
  });
  await page.goto("/");
  await expect(page.locator(".sidebar")).toContainText("Palette probe");
  await page.keyboard.press(`${mod}+k`);
  const palette = page.getByRole("dialog", { name: "Command palette" });
  const field = palette.getByRole("combobox", {
    name: "Find a conversation, command or setting",
  });
  await expect(field).toBeFocused();
  // Letters in order find it; the first match is selected.
  await field.fill("plt prb");
  await expect(palette.getByRole("option").first()).toHaveAttribute(
    "aria-selected",
    "true",
  );
  await expect(palette.getByRole("option").first()).toContainText(
    "Palette probe",
  );
  await field.press("Enter");
  await expect(palette).toHaveCount(0);
  await expect(page.locator(".conversation-head h1")).toHaveText(
    "Palette probe",
  );
  // A command with a screen opens it.
  await page.keyboard.press(`${mod}+k`);
  await field.fill("/memory");
  await field.press("ArrowDown");
  await field.press("ArrowUp");
  await field.press("Enter");
  await expect(page.locator(".conversation-head h1")).toHaveText("Library");
  // Actions show their keys.
  await page.keyboard.press(`${mod}+k`);
  await field.fill("keyboard");
  await expect(palette.getByRole("option").first().locator("kbd")).toHaveText(
    "?",
  );
});

test("? lists the shortcuts, and Alt+arrows step through conversations", async ({
  page,
  host,
  session,
  command,
}) => {
  const { session: second } = await command("create", { cwd: host.project });
  await page.goto(`/#session=${session.id}`);
  await expect(page.locator(".sidebar .session")).toHaveCount(2);
  // In the composer, ? is text.
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("");
  await prompt.press("?");
  await expect(prompt).toHaveValue("?");
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await prompt.fill("");
  await page.locator(".conversation-head h1").click();
  await page.keyboard.press("?");
  const sheet = page.getByRole("dialog", { name: "Keyboard shortcuts" });
  await expect(sheet).toContainText("Command palette");
  await expect(sheet).toContainText("Next conversation");
  await page.keyboard.press("Escape");
  await expect(sheet).toHaveCount(0);
  const ids = [session.id, second.id];
  await page.keyboard.press("Alt+ArrowDown");
  await expect
    .poll(() => new URL(page.url()).hash.replace("#session=", ""))
    .not.toBe(session.id);
  expect(ids).toContain(new URL(page.url()).hash.replace("#session=", ""));
  await page.keyboard.press("Alt+ArrowUp");
  await expect
    .poll(() => new URL(page.url()).hash)
    .toBe(`#session=${session.id}`);
});

test("an empty workspace says what to do, and a folder's coordinator is one step away", async ({
  page,
  host,
}) => {
  await page.goto("/");
  await expect(
    page.getByRole("heading", { name: "What are we working on?" }),
  ).toBeVisible();
  // Nothing to pick yet: the list says how to start, and the page what else
  // there is.
  await expect(
    page.getByRole("navigation", { name: "Conversations" }),
  ).toContainText("No conversations yet");
  const tips = page.locator(".empty .tips li");
  await expect(tips).toHaveCount(3);
  await expect(tips.first()).toContainText("coordinator");
  await expect(tips.nth(1)).toContainText("finds any conversation");

  // The coordinator of a folder that has no conversation yet.
  await page.locator(".empty").getByRole("button").click();
  await page.getByLabel("Directory on the host").fill(host.project);
  await page.getByRole("button", { name: "Open its coordinator" }).click();
  const board = page.getByRole("complementary", { name: "Board" });
  await expect(board).toContainText("Nothing here yet");
  // Its empty chat explains itself, and the input says whom it reaches.
  await expect(page.locator(".transcript .tips")).toContainText(
    "Start a message with a name",
  );
  await expect(page.locator(".transcript .empty")).toContainText(
    "starts threads that do the work",
  );
});

test("the tips of an empty conversation go with its first message", async ({
  page,
  session,
  command,
}) => {
  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
  await page.goto(`/#session=${session.id}`);
  const tips = page.locator(".transcript .tips li");
  await expect(tips).toHaveCount(3);
  await expect(tips.first()).toContainText("lists the commands");
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Hello");
  await prompt.press("Enter");
  await expect(page.locator(".transcript .message").first()).toBeVisible();
  await expect(tips).toHaveCount(0);
});

test("the palette opens from the phone's sidebar", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await page.getByLabel("Open conversations").click();
  await page.getByRole("button", { name: "Command palette" }).click();
  await expect(
    page.getByRole("dialog", { name: "Command palette" }),
  ).toBeVisible();
});

test.describe("pairing by link", () => {
  test.use({ paired: false });

  test("a pairing link connects at once and leaves the address", async ({
    page,
    host,
  }) => {
    await page.goto(`/#pair=${host.code}`);
    await expect(page.locator(".sidebar")).toBeVisible();
    expect(new URL(page.url()).hash).toBe("");
  });

  test("a refused code says it expired", async ({ page }) => {
    await page.goto("/#pair=not-a-code");
    await expect(page.getByRole("alert")).toContainText(
      "Expired — run uagent --web again",
    );
    expect(new URL(page.url()).hash).toBe("");
  });
});
