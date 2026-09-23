import { test, expect } from "./fixtures.js";

const pointer = async (locator, type, pointerId, x, y) =>
  locator.dispatchEvent(type, {
    pointerId,
    pointerType: "touch",
    isPrimary: pointerId === 1,
    clientX: x,
    clientY: y,
  });

test("static UI showcase renders shared flat controls and scales them", async ({
  page,
}, testInfo) => {
  await page.setViewportSize({ width: 1100, height: 900 });
  await page.addInitScript(() => localStorage.setItem("uagent-theme", "dark"));
  await page.goto("/ui.html");

  await expect(
    page.getByRole("heading", { name: "µAgent UI showcase", exact: true }),
  ).toBeVisible();
  const secondary = page.getByRole("button", {
    name: "Secondary action",
    exact: true,
  });
  const select = page.getByRole("combobox").nth(1);
  const styles = await Promise.all(
    [secondary, select].map((control) =>
      control.evaluate((element) => {
        const style = getComputedStyle(element);
        return {
          background: style.backgroundColor,
          border: style.borderTopColor,
          radius: style.borderRadius,
          shadow: style.boxShadow,
        };
      }),
    ),
  );
  expect(styles[0].background).toBe(styles[1].background);
  expect(styles[0].border).toBe("rgba(0, 0, 0, 0)");
  expect(styles[1].border).toBe("rgba(0, 0, 0, 0)");
  expect(styles[0].radius).toBe("0px");
  expect(styles[0].shadow).toBe("none");

  const normalHeight = await secondary.evaluate(
    (element) => element.getBoundingClientRect().height,
  );
  const normalWidth = await page
    .locator(".showcase")
    .evaluate((element) => element.getBoundingClientRect().width);
  await page.getByLabel("Zoom", { exact: true }).fill("50");
  await expect(page.locator("html")).toHaveCSS("--zoom", "0.5");
  const smallHeight = await secondary.evaluate(
    (element) => element.getBoundingClientRect().height,
  );
  expect(smallHeight).toBeLessThan(normalHeight * 0.7);
  const compactWidth = await page
    .locator(".showcase")
    .evaluate((element) => element.getBoundingClientRect().width);
  expect(compactWidth).toBeGreaterThan(normalWidth);

  await page.getByRole("button", { name: "Example menu" }).click();
  await expect(page.getByRole("menu", { name: "Example menu" })).toBeVisible();
  await page.keyboard.press("Escape");
  await page.getByRole("button", { name: "Open dialog" }).click();
  const dialog = page.getByRole("dialog", { name: "Example dialog" });
  await expect(dialog).toBeVisible();
  await dialog.getByRole("button", { name: "Confirm" }).click();
  await expect(dialog).toHaveCount(0);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  await page.screenshot({
    path: testInfo.outputPath("ui-showcase.png"),
    fullPage: true,
  });
});

test.describe("browser input showcase on a phone", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
  });

  test("keeps view gestures separate from trackpad input", async ({ page }) => {
    await page.goto("/ui.html");
    await page.getByRole("button", { name: "Open browser input" }).click();
    const canvas = page.locator(".showcase-browser-input canvas");
    await expect(canvas).toBeVisible();
    await canvas.evaluate((element) => {
      globalThis.browserInputEvents = [];
      for (const type of ["mousedown", "mouseup", "mousemove", "wheel"])
        element.addEventListener(type, (event) => {
          globalThis.browserInputEvents.push({
            type,
            button: event.button,
            buttons: event.buttons,
            x: event.clientX,
            y: event.clientY,
            deltaY: event.deltaY || 0,
          });
        });
    });
    const takeEvents = () =>
      page.evaluate(() => globalThis.browserInputEvents.splice(0));
    const pad = page.getByLabel("Browser trackpad");
    await pad.scrollIntoViewIfNeeded();
    await page.evaluate(
      () =>
        new Promise((resolve) =>
          requestAnimationFrame(() => requestAnimationFrame(resolve)),
        ),
    );
    await takeEvents();
    const box = await pad.boundingBox();
    const trackpadBox = await page.locator(".browser-trackpad").boundingBox();
    expect(trackpadBox.height).toBeGreaterThan(trackpadBox.width / 2);
    const x = box.x + box.width / 2;
    const y = box.y + box.height / 2;

    await pointer(pad, "pointerdown", 1, x, y);
    await pointer(pad, "pointerup", 1, x, y);
    expect((await takeEvents()).slice(0, 2)).toMatchObject([
      { type: "mousedown", button: 0, buttons: 1 },
      { type: "mouseup", button: 0, buttons: 0 },
    ]);

    await pointer(pad, "pointerdown", 2, x - 25, y);
    await pointer(pad, "pointerdown", 3, x + 25, y);
    await pointer(pad, "pointerup", 2, x - 25, y);
    await pointer(pad, "pointerup", 3, x + 25, y);
    expect((await takeEvents()).slice(0, 2)).toMatchObject([
      { type: "mousedown", button: 2, buttons: 2 },
      { type: "mouseup", button: 2, buttons: 0 },
    ]);

    await pointer(pad, "pointerdown", 4, x - 25, y + 30);
    await pointer(pad, "pointerdown", 5, x + 25, y + 30);
    await pointer(pad, "pointermove", 4, x - 25, y - 30);
    await pointer(pad, "pointermove", 5, x + 25, y - 30);
    await pointer(pad, "pointerup", 4, x - 25, y - 30);
    await pointer(pad, "pointerup", 5, x + 25, y - 30);
    expect(await takeEvents()).toEqual(
      expect.arrayContaining([expect.objectContaining({ type: "wheel" })]),
    );

    const left = page.getByRole("button", { name: "Left", exact: true });
    const right = page.getByRole("button", { name: "Right", exact: true });
    const leftBox = await left.boundingBox();
    const rightBox = await right.boundingBox();
    expect(Math.abs(leftBox.width - rightBox.width)).toBeLessThan(1);
    await pointer(
      left,
      "pointerdown",
      6,
      leftBox.x + leftBox.width / 2,
      leftBox.y + leftBox.height / 2,
    );
    await pointer(pad, "pointerdown", 7, x, y);
    await pointer(pad, "pointermove", 7, x + 60, y + 20);
    await pointer(pad, "pointerup", 7, x + 60, y + 20);
    await pointer(
      left,
      "pointerup",
      6,
      leftBox.x + leftBox.width / 2,
      leftBox.y + leftBox.height / 2,
    );
    const drag = await takeEvents();
    expect(drag[0]).toMatchObject({ type: "mousedown", buttons: 1 });
    expect(drag).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ type: "mousemove", buttons: 1 }),
      ]),
    );
    expect(drag.at(-2)).toMatchObject({ type: "mouseup", buttons: 0 });
    const lastMove = drag.findLast(({ type }) => type === "mousemove");
    expect(
      await page.evaluate(
        ({ x, y }) => document.elementFromPoint(x, y)?.tagName,
        lastMove,
      ),
    ).toBe("CANVAS");

    await pointer(
      left,
      "pointerdown",
      8,
      leftBox.x + leftBox.width / 2,
      leftBox.y + leftBox.height / 2,
    );
    await pointer(
      left,
      "pointercancel",
      8,
      leftBox.x + leftBox.width / 2,
      leftBox.y + leftBox.height / 2,
    );
    expect((await takeEvents()).map(({ type }) => type)).toEqual([
      "mousedown",
      "mouseup",
      "mousemove",
    ]);

    await pointer(
      left,
      "pointerdown",
      12,
      leftBox.x + leftBox.width / 2,
      leftBox.y + leftBox.height / 2,
    );
    await page.evaluate(() => dispatchEvent(new Event("blur")));
    expect((await takeEvents()).map(({ type }) => type)).toEqual([
      "mousedown",
      "mouseup",
      "mousemove",
    ]);

    const viewport = page.getByLabel("Browser viewport");
    const viewBox = await viewport.boundingBox();
    const target = page.locator(".showcase-browser-input .browser-rfb");
    await takeEvents();
    await pointer(
      viewport,
      "pointerdown",
      9,
      viewBox.x + viewBox.width * 0.35,
      viewBox.y + viewBox.height / 2,
    );
    await pointer(
      viewport,
      "pointerdown",
      10,
      viewBox.x + viewBox.width * 0.65,
      viewBox.y + viewBox.height / 2,
    );
    await pointer(
      viewport,
      "pointermove",
      9,
      viewBox.x + viewBox.width * 0.2,
      viewBox.y + viewBox.height / 2,
    );
    await pointer(
      viewport,
      "pointermove",
      10,
      viewBox.x + viewBox.width * 0.8,
      viewBox.y + viewBox.height / 2,
    );
    const zoomed = await target.evaluate(
      (element) =>
        element.getBoundingClientRect().width /
        element.parentElement.getBoundingClientRect().width,
    );
    expect(zoomed).toBeGreaterThan(1);
    const zoomTransform = await target.evaluate(
      (element) => element.style.transform,
    );
    await pointer(
      viewport,
      "pointerup",
      9,
      viewBox.x + viewBox.width * 0.2,
      viewBox.y + viewBox.height / 2,
    );
    await pointer(
      viewport,
      "pointerup",
      10,
      viewBox.x + viewBox.width * 0.8,
      viewBox.y + viewBox.height / 2,
    );
    await pointer(viewport, "pointerdown", 11, viewBox.x + 120, viewBox.y + 80);
    await pointer(
      viewport,
      "pointermove",
      11,
      viewBox.x + 150,
      viewBox.y + 100,
    );
    await pointer(viewport, "pointerup", 11, viewBox.x + 150, viewBox.y + 100);
    expect(
      await target.evaluate((element) => element.style.transform),
    ).not.toBe(zoomTransform);
    expect((await takeEvents()).every(({ type }) => type === "mousemove")).toBe(
      true,
    );
  });
});
