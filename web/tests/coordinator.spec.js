// A folder's coordinator opens from its header icon; its board lists the
// folder's sessions beside the chat, and above it on a phone.
import { test, expect } from "./fixtures.js";

for (const [name, viewport] of [
  ["desktop", { width: 1280, height: 800 }],
  ["phone", { width: 390, height: 844 }],
]) {
  test(`coordinator opens with its board (${name})`, async ({
    page,
    session,
  }) => {
    await page.setViewportSize(viewport);
    await page.goto(`/#session=${session.id}`);
    if (name === "phone") await page.getByLabel("Open sessions").click();
    await page.getByLabel("Open this folder's coordinator").first().click();
    const board = page.getByRole("complementary", { name: "Board" });
    await expect(board).toBeVisible();
    await expect(board.locator(".board-row")).toHaveCount(1);
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    // One stable geometry: nothing scrolls the page sideways.
    const overflow = await page.evaluate(
      () => document.documentElement.scrollWidth - window.innerWidth,
    );
    expect(overflow).toBeLessThanOrEqual(0);
    // The help explains what the coordinator is, by tap as well as click.
    await page.getByLabel("What is the coordinator?").click();
    await expect(page.getByText("How it differs from a conversation")).toBeVisible();
    if (process.env.UAGENT_SCREENSHOTS) {
      await page.screenshot({
        path: `${process.env.UAGENT_SCREENSHOTS}/coordinator-${name}.png`,
      });
    }
  });
}
