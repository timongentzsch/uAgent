import { test, expect, selectProbeModel } from "./fixtures.js";

// Composer autogrow: wraps bigger on input and returns to one row on
// send or delete. Regression: the measure-then-guard read scrollHeight
// with the tall height still applied, so shrink was unobservable.
test("composer grows on wrap and shrinks back on send or delete", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  const composer = page.getByLabel("Message or guidance");
  await expect(composer).toBeVisible();
  const height = () =>
    composer.evaluate((element) => element.getBoundingClientRect().height);
  const single = await height();
  await composer.fill(
    "A long line that must wrap onto several rows at phone width. ".repeat(6),
  );
  await expect.poll(height).toBeGreaterThan(single + 20);
  await composer.fill("");
  await expect.poll(height).toBeLessThanOrEqual(single + 1);
  await composer.fill("Short");
  await composer.press("Enter");
  await expect.poll(height).toBeLessThanOrEqual(single + 1);
});

// A one-line draft shares its row with the controls where there is room;
// a wrapped one puts them below it, and a phone always does.
test("composer controls join a one-line draft's row on wide screens", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  const composer = page.getByLabel("Message or guidance");
  await expect(composer).toBeVisible();
  const layout = () =>
    page.evaluate(() => {
      const box = (selector) =>
        document.querySelector(selector).getBoundingClientRect();
      const centre = (rect) => rect.top + rect.height / 2;
      const input = box("#prompt");
      const controls = [
        ".composer .file-button",
        ".composer .model-selector",
        ".permission-control > button",
        ".composer-primary",
      ].map(box);
      return {
        inline: controls.every(
          (rect) => Math.abs(centre(rect) - centre(input)) <= 1,
        ),
        below: controls.every((rect) => rect.top >= input.bottom),
      };
    });
  const inline = { inline: true, below: false };
  const below = { inline: false, below: true };
  await expect.poll(layout).toEqual(inline);
  await composer.fill("One short line");
  await expect.poll(layout).toEqual(inline);
  await composer.fill("First line\nSecond line\nThird line");
  await expect.poll(layout).toEqual(below);
  await expect(composer).toBeFocused();
  await composer.fill("");
  await expect.poll(layout).toEqual(inline);
  await page.setViewportSize({ width: 390, height: 844 });
  await expect.poll(layout).toEqual(below);
  await composer.fill("One short line");
  await expect.poll(layout).toEqual(below);
});
