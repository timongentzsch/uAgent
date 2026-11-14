import { test, expect, online, selectProbeModel } from "./fixtures.js";

test("composer grows on wrap and shrinks back on send or delete", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
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

test("composer controls join a one-line draft's row on wide screens", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
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

test("controls too wide to share the draft's row take the next one", async ({
  page,
  session,
}) => {
  // Wide enough for the shared row, too narrow for a long name beside a draft.
  await page.setViewportSize({ width: 900, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await expect(page.getByLabel("Message or guidance")).toBeVisible();
  await page.evaluate(() => document.fonts.ready);
  const layout = (name) =>
    page.evaluate((name) => {
      const label = document.querySelector(".composer .model-selector > span");
      if (name) label.textContent = name;
      const box = (selector) =>
        document.querySelector(selector).getBoundingClientRect();
      const [draft, form, actions] = [
        "#prompt",
        ".composer > form",
        ".composer-actions",
      ].map(box);
      return {
        draft: Math.round((draft.width / form.width) * 10) / 10,
        below: actions.top >= draft.bottom,
        inside: actions.right <= form.right && actions.left >= form.left,
      };
    }, name);
  expect((await layout()).below).toBe(false);
  // A name that leaves the draft too little room.
  expect(
    await layout("openrouter-free/baidu/ernie-5.5-flash-preview:free:high"),
  ).toEqual({ draft: 1, below: true, inside: true });
  // One wider than the box is cut, not let out of it.
  expect((await layout("a-very-long-model-name/".repeat(12))).inside).toBe(
    true,
  );
});
