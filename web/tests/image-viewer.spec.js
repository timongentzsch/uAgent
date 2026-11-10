// The image viewer: any image fits whole, and the zoom controls, keys and
// ⌘/Ctrl+wheel zoom it like a document viewer.
import { test, expect, online } from "./fixtures.js";

const pointer = (locator, type, pointerId, x, y) =>
  locator.dispatchEvent(type, {
    pointerId,
    pointerType: "touch",
    isPrimary: pointerId === 1,
    clientX: x,
    clientY: y,
  });

for (const [label, viewport, touch] of [
  ["on a desktop", { width: 1440, height: 900 }, false],
  ["on a phone", { width: 390, height: 844 }, true],
]) {
  test.describe(`image viewer ${label}`, () => {
    test.use({ viewport, isMobile: touch, hasTouch: touch });

    test("fills the screen, never upscales, zooms and closes", async ({
      page,
      session,
    }) => {
      await page.goto(`/#session=${session.id}`);
      await online(page);
      // A small picture, and a phone screenshot far taller than the screen.
      const [small, tall] = await page.evaluate(() =>
        Promise.all(
          [
            [400, 300],
            [600, 2400],
          ].map(async ([width, height]) => {
            const canvas = new OffscreenCanvas(width, height);
            const ctx = canvas.getContext("2d");
            ctx.fillStyle = "#3b82f6";
            ctx.fillRect(0, 0, width, height);
            const blob = await canvas.convertToBlob({ type: "image/png" });
            return [...new Uint8Array(await blob.arrayBuffer())];
          }),
        ),
      );
      await page.locator('.composer input[type="file"]').setInputFiles([
        {
          name: "sample.png",
          mimeType: "image/png",
          buffer: Buffer.from(small),
        },
        { name: "tall.png", mimeType: "image/png", buffer: Buffer.from(tall) },
      ]);
      await page.getByRole("button", { name: "View sample.png" }).click();
      const dialog = page.getByRole("dialog", { name: "sample.png" });
      const image = dialog.getByRole("img", { name: "sample.png" });
      await expect(image).toBeVisible();
      // Full bleed: the viewer is the screen, with no frame around it.
      const box = await dialog.boundingBox();
      expect([box.x, box.y, box.width, box.height].map(Math.round)).toEqual([
        0,
        0,
        viewport.width,
        viewport.height,
      ]);
      // Natural size when it fits; scaled down (never up) when it does not.
      const shown = await image.boundingBox();
      expect(shown.width).toBeLessThanOrEqual(400 + 0.5);
      expect(Math.abs(shown.width / shown.height - 4 / 3)).toBeLessThan(0.01);
      const surface = dialog.getByRole("group", { name: "sample.png" });
      const center = [shown.x + shown.width / 2, shown.y + shown.height / 2];
      const zoomed = async () =>
        (await image.boundingBox()).width / shown.width;
      if (touch) {
        // Pinch zooms the image; a double-tap returns it to fit.
        await pointer(surface, "pointerdown", 1, center[0] - 20, center[1]);
        await pointer(surface, "pointerdown", 2, center[0] + 20, center[1]);
        await pointer(surface, "pointermove", 1, center[0] - 60, center[1]);
        await pointer(surface, "pointermove", 2, center[0] + 60, center[1]);
        await pointer(surface, "pointerup", 1, center[0] - 60, center[1]);
        await pointer(surface, "pointerup", 2, center[0] + 60, center[1]);
        expect(await zoomed()).toBeGreaterThan(2.5);
        // Real taps (captured, so later events target the surface) on the
        // image double-tap back to fit instead of closing the viewer.
        await page.touchscreen.tap(...center);
        await page.touchscreen.tap(...center);
        await expect.poll(zoomed).toBeCloseTo(1, 1);
        await expect(dialog).toBeVisible();
      } else {
        // Ctrl+scroll is a trackpad pinch; a double-click toggles zoom.
        await page.mouse.move(...center);
        await page.keyboard.down("Control");
        await page.mouse.wheel(0, -100);
        await page.keyboard.up("Control");
        await expect.poll(zoomed).toBeGreaterThan(2);
        await page.mouse.dblclick(...center);
        await expect.poll(zoomed).toBeCloseTo(1, 1);
        await expect(dialog).toBeVisible();
      }
      // A tap on the empty space around the image closes the viewer.
      const view = await surface.boundingBox();
      if (touch) {
        await pointer(surface, "pointerdown", 5, view.x + 4, view.y + 4);
        await pointer(surface, "pointerup", 5, view.x + 4, view.y + 4);
      } else await page.mouse.click(view.x + 4, view.y + 4);
      await expect(dialog).toHaveCount(0);
      if (touch) return;

      // The tall one fits whole, and the controls and keys zoom it too.
      await page.getByRole("button", { name: "View tall.png" }).click();
      const viewer = page.getByRole("dialog", { name: "tall.png" });
      const screenshot = viewer.getByRole("img", { name: "tall.png" });
      await expect(screenshot).toBeVisible();
      const controls = viewer.getByRole("group", { name: "Zoom" });
      const level = controls.locator("output");
      // Image decoding and the fit observer follow the initial paint.
      await expect
        .poll(async () => Number((await level.textContent()).replace("%", "")))
        .toBeLessThan(100);
      const fitted = await screenshot.boundingBox();
      const frame = await viewer
        .getByRole("group", { name: "tall.png" })
        .boundingBox();
      expect(fitted.y).toBeGreaterThanOrEqual(frame.y - 0.5);
      expect(fitted.y + fitted.height).toBeLessThanOrEqual(
        frame.y + frame.height + 0.5,
      );
      const scaled = async () =>
        (await screenshot.boundingBox()).width / fitted.width;
      // The level is against the real size: 2400px shown in a shorter frame.
      const fit = Number((await level.textContent()).replace("%", ""));
      expect(fit).toBeLessThan(100);
      await controls.getByRole("button", { name: "Zoom in" }).click();
      await expect.poll(scaled).toBeCloseTo(1.25, 1);
      await expect(level).not.toHaveText(`${fit}%`);
      await controls.getByRole("button", { name: "Fit" }).click();
      await expect.poll(scaled).toBeCloseTo(1, 1);
      await expect(level).toHaveText(`${fit}%`);
      await page.mouse.move(
        fitted.x + fitted.width / 2,
        fitted.y + fitted.height / 2,
      );
      await page.keyboard.down("Meta");
      await page.mouse.wheel(0, -100);
      await page.keyboard.up("Meta");
      await expect.poll(scaled).toBeGreaterThan(1.5);
      await page.keyboard.press("ControlOrMeta+0");
      await expect.poll(scaled).toBeCloseTo(1, 1);
      await page.keyboard.press("ControlOrMeta+=");
      await expect.poll(scaled).toBeCloseTo(1.25, 1);
    });
  });
}
