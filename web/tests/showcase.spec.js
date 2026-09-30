import { showcaseTest as test, expect, SHOWCASE_URL } from "./fixtures.js";

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
}) => {
  await page.setViewportSize({ width: 1100, height: 900 });
  await page.addInitScript(() => localStorage.setItem("uagent-theme", "dark"));
  await page.goto(SHOWCASE_URL);

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
  // A field draws its edge (WCAG 1.4.11); a button's label marks it.
  expect(styles[1].border).not.toBe("rgba(0, 0, 0, 0)");
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
});

test.describe("browser input showcase on a phone", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
  });

  test("direct touch maps to exact pixels and never leaves a button held", async ({
    page,
  }) => {
    await page.goto(SHOWCASE_URL);
    await page.getByRole("button", { name: "Open browser input" }).click();
    const viewport = page.getByLabel("Browser viewport");
    const canvas = viewport.locator(".browser-rfb canvas");
    await expect(canvas).toBeVisible();
    // What a VNC server would receive: [x, y, button mask] at framebuffer
    // pixels of the 800x500 sample screen.
    const takeEvents = () =>
      page.evaluate(() =>
        (globalThis.browserPointer ?? []).splice(0).map(([x, y, mask]) => ({
          x,
          y,
          mask,
        })),
      );
    const box = await canvas.boundingBox();
    const at = (fx, fy) => [box.x + box.width * fx, box.y + box.height * fy];

    // A tap is a click at the pixel under the finger.
    await pointer(viewport, "pointerdown", 1, ...at(0.5, 0.5));
    await pointer(viewport, "pointerup", 1, ...at(0.5, 0.5));
    const tap = await takeEvents();
    expect(tap.map(({ mask }) => mask)).toEqual([0, 1, 0]);
    expect(Math.abs(tap[0].x - 400)).toBeLessThanOrEqual(2);
    expect(Math.abs(tap[0].y - 250)).toBeLessThanOrEqual(2);

    // A held press is released on cancel and when the window loses focus.
    for (const end of ["pointercancel", "blur"]) {
      await pointer(viewport, "pointerdown", 2, ...at(0.3, 0.3));
      await page.waitForTimeout(500);
      if (end === "blur")
        await page.evaluate(() => dispatchEvent(new Event("blur")));
      else await pointer(viewport, end, 2, ...at(0.3, 0.3));
      expect((await takeEvents()).map(({ mask }) => mask)).toEqual([1, 0]);
    }

    // A held drag that ends past the screen's edge still lets go, there.
    await pointer(viewport, "pointerdown", 4, ...at(0.5, 0.9));
    await page.waitForTimeout(500);
    const below = [box.x + box.width / 2, box.y + box.height + 40];
    await pointer(viewport, "pointermove", 4, ...below);
    await pointer(viewport, "pointerup", 4, ...below);
    const edge = await takeEvents();
    expect(edge.map(({ mask }) => mask)).toEqual([1, 1, 0]);
    expect(edge.at(-1).y).toBe(499);

    // A tap off the letterboxed screen sends nothing.
    const view = await viewport.boundingBox();
    const outside = [view.x + view.width / 2, view.y + view.height - 4];
    if (outside[1] > box.y + box.height) {
      await pointer(viewport, "pointerdown", 3, ...outside);
      await pointer(viewport, "pointerup", 3, ...outside);
      expect(await takeEvents()).toEqual([]);
    }
  });
});

for (const [label, viewport, touch] of [
  ["on a desktop", { width: 1440, height: 900 }, false],
  ["on a phone", { width: 390, height: 844 }, true],
]) {
  test.describe(`image viewer ${label}`, () => {
    test.use({ viewport, isMobile: touch, hasTouch: touch });

    test("fills the screen, never upscales, zooms and closes", async ({
      page,
    }) => {
      await page.goto(SHOWCASE_URL);
      await page.getByRole("button", { name: "Open image viewer" }).click();
      const dialog = page.getByRole("dialog", { name: "sample.svg" });
      const image = dialog.getByRole("img", { name: "sample.svg" });
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
      const surface = dialog.getByRole("group", { name: "sample.svg" });
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
    });
  });
}

// The ask decision on the showcase: no host, so its commands are recorded
// and the attachment upload is answered here.
const ASSET_ID = "0123456789abcdef0123456789abcdef";
for (const width of [1440, 900, 390]) {
  test(`ask questions answer with options, Other and an image at ${width}px`, async ({
    page,
    browserName,
  }) => {
    await page.setViewportSize({ width, height: 900 });
    await page.route("**/api/sessions/showcase/attachments?*", (route) =>
      route.fulfill({
        json: { id: ASSET_ID, name: "shot.png", bytes: 68, image: true },
      }),
    );
    await page.route("**/api/sessions/showcase/assets/*", (route) =>
      route.fulfill({
        contentType: "image/svg+xml",
        body: '<svg xmlns="http://www.w3.org/2000/svg" width="4" height="3"/>',
      }),
    );
    await page.goto(SHOWCASE_URL);
    const commands = () => page.evaluate(() => globalThis.commands ?? []);
    const ask = page.getByRole("region", { name: "Pending decision" });
    await ask.scrollIntoViewIfNeeded();
    // Routed to the coordinator, the form still answers: the first wins.
    await expect(
      ask.getByRole("heading", { name: "The coordinator is deciding" }),
    ).toBeVisible();
    const submit = ask.getByRole("button", { name: "Submit" });
    await expect(submit).toBeDisabled();

    await ask.getByRole("button", { name: "Cancel" }).click();
    await expect.poll(commands).toEqual([
      {
        kind: "reply",
        interaction_id: "showcase-ask",
        text: "",
        cancelled: true,
      },
    ]);

    // Single choice: arrow keys move the choice; Other excludes the options.
    const storage = ask.getByRole("group", { name: /Storage/ });
    const memory = storage.getByRole("radio", { name: /^Memory/ });
    await memory.check();
    // Safari does not focus a clicked radio.
    await memory.focus();
    await page.keyboard.press("ArrowDown");
    await expect(storage.getByRole("radio", { name: /^Disk/ })).toBeChecked();
    await storage.getByRole("radio", { name: /^Other/ }).check();
    await expect(
      storage.getByRole("radio", { name: /^Disk/ }),
    ).not.toBeChecked();
    const other = storage.getByRole("textbox", {
      name: "Other answer: Storage",
    });

    // Multiple choice, with an image.
    const platforms = ask.getByRole("group", { name: /Platforms/ });
    await platforms.getByRole("checkbox", { name: /^macOS/ }).check();
    await platforms.getByRole("checkbox", { name: /^Linux/ }).check();
    await platforms.getByLabel("Attach image: Platforms").setInputFiles({
      name: "shot.png",
      mimeType: "image/png",
      buffer: Buffer.alloc(68),
    });
    await expect(
      platforms.getByRole("button", { name: "Remove shot.png" }),
    ).toBeVisible();

    // Options the agent showed come with its image and preview; the image's
    // alt text is the option's description.
    const layout = ask.getByRole("group", { name: /Layout/ });
    await expect(
      layout.getByRole("img", {
        name: "Sections listed on the left, one open at a time.",
      }),
    ).toBeVisible();
    await expect(layout.locator(".ask-preview")).toHaveCount(2);
    await layout.getByRole("radio", { name: /^Tabs/ }).check();

    // Other needs its text before the answers are complete.
    await expect(submit).toBeDisabled();
    await other.fill("SQLite");
    await expect(submit).toBeEnabled();
    if (process.env.UAGENT_SCREENSHOTS)
      await ask.screenshot({
        path: `${process.env.UAGENT_SCREENSHOTS}/ask-${browserName}-${width}.png`,
      });

    // Nothing spills sideways at this width.
    expect(
      await ask.evaluate((section) =>
        [section, ...section.querySelectorAll("*")].every(
          (node) =>
            node.getBoundingClientRect().right <= innerWidth + 0.5 &&
            node.scrollWidth <= node.clientWidth + 1,
        ),
      ),
    ).toBe(true);

    await submit.click();
    await expect.poll(async () => (await commands()).length).toBe(2);
    const reply = (await commands())[1];
    expect(reply).toMatchObject({
      kind: "reply",
      interaction_id: "showcase-ask",
      attachment_ids: [ASSET_ID],
    });
    expect(JSON.parse(reply.text)).toEqual([
      { choices: [], other: "SQLite" },
      { choices: ["macOS", "Linux"], other: "", attachment_id: ASSET_ID },
      { choices: ["Tabs"], other: "" },
    ]);
  });
}
