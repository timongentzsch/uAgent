// Image markup: strokes and noted pins baked into a copy that replaces the
// draft image, driven by mouse on desktop and by touch on a phone.
import { test, expect } from "./fixtures.js";

async function open(page, session) {
  await page.goto(`/#session=${session.id}`);
  await expect(page.getByLabel("Message or guidance")).toBeVisible();
  // A plain grey screenshot stand-in, large enough to hold the marks.
  const png = await page.evaluate(async () => {
    const canvas = new OffscreenCanvas(320, 200);
    const ctx = canvas.getContext("2d");
    ctx.fillStyle = "#808080";
    ctx.fillRect(0, 0, 320, 200);
    const blob = await canvas.convertToBlob({ type: "image/png" });
    return [...new Uint8Array(await blob.arrayBuffer())];
  });
  await page.locator('.composer input[type="file"]').setInputFiles({
    name: "shot.png",
    mimeType: "image/png",
    buffer: Buffer.from(png),
  });
  await page.getByRole("button", { name: "View shot.png" }).click();
  const viewer = page.getByRole("dialog", { name: "shot.png" });
  await viewer.getByRole("button", { name: "Annotate" }).click();
  const canvas = viewer.getByLabel("Annotate shot.png");
  await expect(canvas).toBeVisible();
  return { viewer, canvas, box: await canvas.boundingBox() };
}

// Counts markup-red pixels in the attached copy.
async function marked(page) {
  const chip = page.locator(".composer .file-chip", {
    hasText: "shot-annotated.png",
  });
  await expect(chip).toBeVisible();
  await expect(
    page.locator(".composer .file-chip", { hasText: /^shot\.png/ }),
  ).toHaveCount(0);
  const src = await chip.locator("img").getAttribute("src");
  return page.evaluate(async (src) => {
    const bitmap = await createImageBitmap(await (await fetch(src)).blob());
    const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
    const ctx = canvas.getContext("2d");
    ctx.drawImage(bitmap, 0, 0);
    const { data } = ctx.getImageData(0, 0, bitmap.width, bitmap.height);
    let red = 0;
    for (let i = 0; i < data.length; i += 4)
      if (data[i] > 200 && data[i + 1] < 100 && data[i + 2] < 100) red++;
    return { width: bitmap.width, height: bitmap.height, red };
  }, src);
}

test("mouse strokes, undo and a noted pin attach as a replacing copy", async ({
  page,
  session,
}) => {
  const { viewer, canvas, box } = await open(page, session);
  const undo = viewer.getByRole("button", { name: "Undo" });
  const drag = async () => {
    await page.mouse.move(box.x + box.width * 0.2, box.y + box.height * 0.3);
    await page.mouse.down();
    await page.mouse.move(box.x + box.width * 0.6, box.y + box.height * 0.5, {
      steps: 8,
    });
    await page.mouse.up();
  };
  await drag();
  await expect(undo).toBeEnabled();
  await page.keyboard.press("ControlOrMeta+z");
  await expect(undo).toBeDisabled();
  await drag();

  await viewer.getByRole("button", { name: "Pin" }).click();
  await canvas.click({ position: { x: box.width * 0.7, y: box.height * 0.7 } });
  const note = viewer.getByLabel("Note for pin 1");
  await note.fill("make this blue");
  // Escape closes the bubble, not the viewer.
  await note.press("Escape");
  await expect(note).toHaveCount(0);
  await expect(viewer).toBeVisible();

  await viewer.getByRole("button", { name: "Attach" }).click();
  await expect(viewer).toHaveCount(0);
  const result = await marked(page);
  expect(result).toMatchObject({ width: 320, height: 200 });
  expect(result.red).toBeGreaterThan(500);
});

test.describe("touch", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
  });

  test("finger draws, a second finger cancels, a tap drops a pin", async ({
    page,
    session,
  }) => {
    const { viewer, canvas, box } = await open(page, session);
    const undo = viewer.getByRole("button", { name: "Undo" });
    const cdp = await page.context().newCDPSession(page);
    const touch = (type, points) =>
      cdp.send("Input.dispatchTouchEvent", {
        type,
        touchPoints: points.map(([x, y], id) => ({
          x: box.x + box.width * x,
          y: box.y + box.height * y,
          id,
        })),
      });

    // A second finger landing mid-stroke abandons it.
    await touch("touchStart", [[0.2, 0.3]]);
    await touch("touchMove", [[0.4, 0.4]]);
    await touch("touchStart", [
      [0.4, 0.4],
      [0.7, 0.7],
    ]);
    await touch("touchEnd", []);
    await expect(undo).toBeDisabled();

    await touch("touchStart", [[0.2, 0.3]]);
    for (const step of [0.3, 0.4, 0.5, 0.6])
      await touch("touchMove", [[step, step]]);
    await touch("touchEnd", []);
    await expect(undo).toBeEnabled();

    // Two fingers zoom the image instead of drawing.
    const zoom = viewer.getByRole("group", { name: "Zoom" });
    const level = await zoom.locator("output").textContent();
    await touch("touchStart", [
      [0.4, 0.5],
      [0.6, 0.5],
    ]);
    for (const spread of [0.15, 0.25, 0.35])
      await touch("touchMove", [
        [0.5 - spread, 0.5],
        [0.5 + spread, 0.5],
      ]);
    await touch("touchEnd", []);
    await expect(zoom.locator("output")).not.toHaveText(level);
    await zoom.getByRole("button", { name: "Fit" }).click();
    await expect(zoom.locator("output")).toHaveText(level);

    // The toolbar is not what this test covers; right after a synthetic
    // multi-finger sequence, CI's emulated gesture detector can swallow a
    // button tap. The canvas itself is still driven by touch.
    const pin = viewer.getByRole("button", { name: "Pin" });
    await pin.click();
    await expect(pin).toHaveAttribute("aria-pressed", "true");
    await canvas.tap({ position: { x: box.width * 0.8, y: box.height * 0.2 } });
    const note = viewer.getByLabel("Note for pin 1");
    await note.fill("remove this");
    const bubble = await note.boundingBox();
    expect(bubble.x).toBeGreaterThanOrEqual(0);
    expect(bubble.x + bubble.width).toBeLessThanOrEqual(390);
    await note.press("Enter");
    expect(await page.evaluate(() => scrollX + scrollY)).toBe(0);

    await viewer.getByRole("button", { name: "Attach" }).tap();
    await expect(viewer).toHaveCount(0);
    expect((await marked(page)).red).toBeGreaterThan(300);
  });
});
