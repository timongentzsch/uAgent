// A member's avatar answers who it is: a card on hover or focus with a
// pointer, the same in a sheet on a touch screen.
import { test, expect, openPanel } from "./fixtures.js";

async function shot(page, name) {
  if (process.env.UAGENT_SCREENSHOTS) {
    await page.screenshot({
      path: `${process.env.UAGENT_SCREENSHOTS}/${name}.png`,
    });
  }
}

// Opens the panel and has Ada join its chat.
async function seatAda(page, session, command, request) {
  const id = await openPanel(page, session);
  let meta;
  await expect
    .poll(async () => {
      meta = (await (await request.get(`/api/sessions/${id}`)).json()).metadata;
      return meta.generation;
    })
    .toBeTruthy();
  await command("model", {
    session_id: id,
    generation: meta.generation,
    operation: "select",
    model: "mock/model-b",
  });
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Chat probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(
    page.locator(".transcript .message", {
      has: page.locator(".actor", { hasText: "Ada" }),
    }),
  ).toBeVisible();
  return { id, prompt };
}

test("a member's avatar shows its card on hover and on focus", async ({
  page,
  session,
  command,
  request,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  const { id, prompt } = await seatAda(page, session, command, request);
  const sidebar = page.locator(".sidebar");
  const card = page.getByRole("dialog", { name: "Ada" });
  const avatar = sidebar.getByRole("button", { name: "Ada, member" });

  // Pointed at, it says who Ada is and what it was told; moving away or
  // Escape closes it.
  await avatar.hover();
  await expect(card).toBeVisible();
  await expect(card).toContainText("Member · model-b");
  await expect(card).toContainText("PERSONA");
  await expect(card).toContainText("You give second opinions.");
  await expect(card).not.toContainText("SKILLS");
  await shot(page, "member-card");
  await page.mouse.move(700, 450);
  await expect(card).toHaveCount(0);
  await avatar.hover();
  await expect(card).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(card).toHaveCount(0);

  // The moderator's says what it is instead of a persona.
  await sidebar.getByRole("button", { name: "Moderator, moderator" }).hover();
  await expect(page.getByRole("dialog", { name: "Moderator" })).toContainText(
    "Hands work to threads; never edits files.",
  );
  await page.mouse.move(700, 450);

  // Focused from the keyboard it opens too, and Tab reaches its buttons.
  await avatar.focus();
  await expect(card).toBeVisible();
  await page.keyboard.press("Tab");
  await expect(card.getByRole("button", { name: "Ask Ada" })).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(card).toHaveCount(0);
  await expect(avatar).toBeFocused();

  // Full prompt is the one Show prompt opens.
  await avatar.hover();
  await card.getByRole("button", { name: "Full prompt" }).click();
  await expect(page.getByRole("dialog")).toContainText(
    "You are Ada, a member of the panel",
  );
  await page.keyboard.press("Escape");

  // Its menu renames; Ask starts a message to it in the panel's composer.
  await avatar.hover();
  await card.getByRole("button", { name: "Menu for Ada" }).click();
  await page.getByRole("menuitem", { name: "Rename" }).click();
  await expect(page.getByLabel("Member’s name")).toBeVisible();
  await page.keyboard.press("Escape");
  await avatar.hover();
  await card.getByRole("button", { name: "Ask Ada" }).click();
  await expect(prompt).toHaveValue("Ada, ");
  await expect(prompt).toBeFocused();
  expect(new URL(page.url()).hash).toContain(`session=${id}`);
});

test.describe("on a touch screen", () => {
  test.use({ hasTouch: true, viewport: { width: 390, height: 844 } });

  test("tapping a member's avatar opens its card in a sheet", async ({
    page,
    session,
    command,
    request,
  }) => {
    await seatAda(page, session, command, request);
    expect(
      await page.evaluate(() => matchMedia("(pointer: coarse)").matches),
    ).toBe(true);
    await page
      .locator(".transcript .message")
      .getByRole("button", { name: "Ada, member" })
      .first()
      .tap();
    const sheet = page.getByRole("dialog", { name: "Ada" });
    await expect(sheet).toContainText("You give second opinions.");
    // Its buttons stand at the control height, side by side.
    const control = await page.evaluate(
      () =>
        parseFloat(getComputedStyle(document.documentElement).fontSize) * 2.2,
    );
    for (const name of ["Ask Ada", "Full prompt", "Menu for Ada"]) {
      const box = await sheet.getByRole("button", { name }).boundingBox();
      expect(box.height).toBeGreaterThanOrEqual(control - 1);
      expect(box.x + box.width).toBeLessThanOrEqual(390);
    }
    await shot(page, "member-sheet");
    await sheet.getByRole("button", { name: "Ask Ada" }).tap();
    await expect(sheet).toHaveCount(0);
    await expect(page.getByLabel("Message or guidance")).toHaveValue("Ada, ");
  });
});
