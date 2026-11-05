// What enters leaves the same way: the docked Files pane slides in within
// its column, plays its exit when closed (unreachable while it goes), and
// returns focus to what opened it. With motion off it simply goes.
import { test, expect, headerAction } from "./fixtures.js";

// What the pane has started to animate since `forget`: the events come with
// the next frame, so a check polls for them.
const moved = (page) =>
  page.evaluate(() => [...new Set(window.paneMoves)].sort());
const forget = (page) => page.evaluate(() => (window.paneMoves = []));

for (const motion of ["no-preference", "reduce"])
  test(`the Files pane enters, leaves and returns focus (${motion})`, async ({
    page,
    session,
  }) => {
    const expected = motion === "reduce" ? [] : ["opacity", "translate"];
    await page.emulateMedia({ reducedMotion: motion });
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.goto(`/#session=${session.id}`);
    await page.evaluate(() => {
      window.paneMoves = [];
      document.addEventListener("transitionrun", (event) => {
        if (event.target.matches?.(".side-pane"))
          window.paneMoves.push(event.propertyName);
      });
    });
    const pane = page.getByRole("region", { name: "Files" });
    await headerAction(page, "Files");
    await expect(pane).toBeVisible();
    await expect.poll(() => moved(page)).toEqual(expected);
    await forget(page);
    await pane.getByRole("button", { name: "Close files" }).click();
    if (motion !== "reduce") {
      // Leaving: still drawn, but nothing in it can be reached.
      const leaving = page.locator(".side-pane[data-leaving]");
      await expect(leaving).toHaveCount(1);
      expect(await leaving.evaluate((node) => node.inert)).toBe(true);
    }
    await expect(pane).toHaveCount(0);
    await expect.poll(() => moved(page)).toEqual(expected);
    await expect(
      page.getByRole("button", { name: /^Files\b/ }).filter({ visible: true }),
    ).toBeFocused();
  });
