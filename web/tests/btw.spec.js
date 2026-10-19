import { test, expect, online } from "./fixtures.js";

test.beforeEach(({ command, session }) =>
  command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  }),
);

for (const width of [320, 1280]) {
  test(`btw tools and follow-ups stay beside the parent (${width})`, async ({
    page,
    session,
  }, testInfo) => {
    await page.setViewportSize({ width, height: 844 });
    await page.goto(`/#session=${session.id}`);
    await online(page);
    const prompt = page.getByLabel("Message or guidance");
    await prompt.fill("/btw Side tool probe");
    await page.getByRole("button", { name: "Send", exact: true }).click();
    const side = page.getByRole("complementary", { name: "Side conversation" });
    await expect(
      side.getByRole("button", { name: "Allow once", exact: true }),
    ).toBeVisible();
    await side.getByRole("button", { name: "Allow once", exact: true }).click();
    await expect(side).toContainText("SIDE-TOOL-COMPLETE");
    await expect(page.locator(".transcript")).not.toContainText(
      "SIDE-TOOL-COMPLETE",
    );
    await page.reload();
    await online(page);
    await expect(side).toContainText("SIDE-TOOL-COMPLETE");
    await side
      .getByLabel("Follow up on side conversation")
      .fill("Side followup probe");
    if (width === 1280)
      await side.getByLabel("Follow up on side conversation").press("Enter");
    else
      await side
        .getByRole("button", { name: "Send follow-up", exact: true })
        .click();
    await expect(side).toContainText("SIDE-FOLLOWUP-COMPLETE");
    await expect(side).toContainText("SIDE-TOOL-COMPLETE");
    await expect(page).toHaveURL(new RegExp(`session=${session.id}$`));
    const layout = await side.evaluate((node) => ({
      overflow: document.documentElement.scrollWidth > innerWidth,
      width: node.getBoundingClientRect().width,
      height: node.getBoundingClientRect().height,
      button: node
        .querySelector('button[type="submit"]')
        .getBoundingClientRect().height,
    }));
    expect(layout.overflow).toBe(false);
    expect(layout.width).toBeLessThanOrEqual(width);
    expect(layout.height).toBeLessThan(500);
    if (width === 320) expect(layout.button).toBeGreaterThanOrEqual(44);
    await page.screenshot({ path: testInfo.outputPath("btw.png") });
    await side.getByRole("button", { name: "Open side conversation" }).click();
    await expect(page).not.toHaveURL(new RegExp(`session=${session.id}$`));
    await expect(page.locator(".transcript")).toContainText(
      "SIDE-FOLLOWUP-COMPLETE",
    );
  });
}
