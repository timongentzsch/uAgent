// What enters leaves the same way: the docked Files pane's column grows as
// it fades in, so the chat narrows with it, and shrinks again when closed
// (unreachable while it goes); focus returns to what opened it. With motion
// off it simply goes.
import { test, expect, headerAction } from "./fixtures.js";

// What the pane has animated since `forget`, and how its width went.
const moved = (page) =>
  page.evaluate(() => [...new Set(window.paneMoves)].sort());
const forget = (page) => page.evaluate(() => (window.paneMoves = []));

for (const motion of ["no-preference", "reduce"])
  test(`the Files pane enters, leaves and returns focus (${motion})`, async ({
    page,
    session,
  }) => {
    const expected =
      motion === "reduce" ? [] : ["minWidth", "opacity", "overflow", "width"];
    await page.emulateMedia({ reducedMotion: motion });
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.goto(`/#session=${session.id}`);
    await page.evaluate(() => {
      window.paneMoves = [];
      window.paneWidths = [];
      const animate = Element.prototype.animate;
      Element.prototype.animate = function (frames, options) {
        if (this.matches(".side-pane")) {
          for (const frame of frames)
            for (const key of Object.keys(frame))
              if (!["offset", "easing", "composite"].includes(key))
                window.paneMoves.push(key);
          window.paneWidths.push(frames.map((frame) => frame.width));
        }
        return animate.call(this, frames, options);
      };
    });
    const pane = page.getByRole("region", { name: "Files" });
    await headerAction(page, "Files");
    await expect(pane).toBeVisible();
    await expect.poll(() => moved(page)).toEqual(expected);
    if (motion !== "reduce") {
      // From nothing to its full width: the chat gives way as it grows.
      const [opened] = await page.evaluate(() => window.paneWidths);
      expect(opened[0]).toBe("0px");
      expect(Number.parseFloat(opened[1])).toBeGreaterThan(300);
    }
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
