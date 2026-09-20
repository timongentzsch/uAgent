import { test, expect } from "./fixtures.js";

// Composer autogrow: wraps bigger on input and returns to one row on
// send or delete. Regression: the measure-then-guard read scrollHeight
// with the tall height still applied, so shrink was unobservable.
test("composer grows on wrap and shrinks back on send or delete", async ({
  page,
  session,
  command,
}) => {
  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
  await command("permissions", {
    session_id: session.id,
    generation: session.generation,
    mode: "yolo",
  });
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
