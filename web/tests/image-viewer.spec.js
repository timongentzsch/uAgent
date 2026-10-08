// The image viewer: any image fits whole, and the zoom controls, keys and
// ⌘/Ctrl+wheel zoom it like a document viewer.
import { test, expect, online } from "./fixtures.js";

test("a tall image fits whole and zooms by controls, wheel and keys", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  // A phone screenshot: far taller than the screen.
  const png = await page.evaluate(async () => {
    const canvas = new OffscreenCanvas(600, 2400);
    const ctx = canvas.getContext("2d");
    ctx.fillStyle = "#0080ff";
    ctx.fillRect(0, 0, 600, 2400);
    const blob = await canvas.convertToBlob({ type: "image/png" });
    return [...new Uint8Array(await blob.arrayBuffer())];
  });
  await page.locator('.composer input[type="file"]').setInputFiles({
    name: "tall.png",
    mimeType: "image/png",
    buffer: Buffer.from(png),
  });
  await page.getByRole("button", { name: "View tall.png" }).click();
  const viewer = page.getByRole("dialog", { name: "tall.png" });
  const image = viewer.getByRole("img", { name: "tall.png" });
  const surface = viewer.getByRole("group", { name: "tall.png" });
  await expect(image).toBeVisible();

  const shown = await image.boundingBox();
  const frame = await surface.boundingBox();
  expect(shown.y).toBeGreaterThanOrEqual(frame.y - 0.5);
  expect(shown.y + shown.height).toBeLessThanOrEqual(
    frame.y + frame.height + 0.5,
  );

  const controls = viewer.getByRole("group", { name: "Zoom" });
  const level = controls.locator("output");
  const zoomed = async () => (await image.boundingBox()).width / shown.width;
  // The level is against the real size: 2400px shown in a shorter frame.
  const fit = Number((await level.textContent()).replace("%", ""));
  expect(fit).toBeLessThan(100);

  await controls.getByRole("button", { name: "Zoom in" }).click();
  await expect.poll(zoomed).toBeCloseTo(1.25, 1);
  await expect(level).not.toHaveText(`${fit}%`);
  await controls.getByRole("button", { name: "Fit" }).click();
  await expect.poll(zoomed).toBeCloseTo(1, 1);
  await expect(level).toHaveText(`${fit}%`);

  await page.mouse.move(shown.x + shown.width / 2, shown.y + shown.height / 2);
  await page.keyboard.down("Meta");
  await page.mouse.wheel(0, -100);
  await page.keyboard.up("Meta");
  await expect.poll(zoomed).toBeGreaterThan(1.5);
  await page.keyboard.press("ControlOrMeta+0");
  await expect.poll(zoomed).toBeCloseTo(1, 1);
  await page.keyboard.press("ControlOrMeta+=");
  await expect.poll(zoomed).toBeCloseTo(1.25, 1);
});
