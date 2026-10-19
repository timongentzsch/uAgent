import { test, expect, withoutServiceWorker } from "./fixtures.js";

test("mobile chrome keeps an opaque safe area and applies appearance before app startup", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.emulateMedia({ colorScheme: "dark" });
  await page.addInitScript(() => {
    localStorage.setItem("uagent-theme", "light");
    localStorage.setItem("uagent-zoom", "50");
  });
  let release;
  const startup = new Promise((resolve) => (release = resolve));
  await page.route("**/assets/index-*.js", async (route) => {
    await startup;
    await route.continue();
  });
  try {
    await page.goto("/", { waitUntil: "commit" });
    await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
    await expect(page.locator("html")).toHaveCSS("--zoom", "0.5");
    await expect(page.locator("html")).toHaveCSS(
      "--conversation-measure",
      "2080px",
    );
    await expect(page.locator("body")).toHaveCSS(
      "background-color",
      "rgb(255, 255, 255)",
    );
    await expect(page.locator("#app")).toBeEmpty();
    await expect(page.locator('meta[name="theme-color"]')).toHaveAttribute(
      "content",
      "#ffffff",
    );
    await page.evaluate(() => {
      localStorage.setItem("uagent-zoom", "100");
      document.documentElement.style.setProperty("--zoom", "1");
      document.documentElement.style.setProperty(
        "--conversation-measure",
        "1040px",
      );
    });
  } finally {
    release();
  }
  await expect(
    page.getByRole("heading", { name: "Your workspace" }),
  ).toBeVisible();
  // Desktop WebKit does not expose an iPhone's system inset or status-bar paint.
  // Exercise our safe-area layout independently of that native rendering.
  await page.evaluate(() =>
    document.documentElement.style.setProperty("--safe-top", "47px"),
  );
  for (const selector of ["body", "#app", ".conversation-head"])
    await expect(page.locator(selector)).toHaveCSS(
      "background-color",
      "rgb(255, 255, 255)",
    );
  await expect(page.locator("#app")).toHaveCSS("padding-top", "47px");
  await page.evaluate(() =>
    document.documentElement.style.setProperty("--safe-bottom", "34px"),
  );
  for (let i = 0; i < 2; i++) {
    await page
      .getByRole("button", { name: "Open conversations", exact: true })
      .click();
    const drawer = page.getByRole("dialog", {
      name: "Conversations",
      exact: true,
    });
    await expect(drawer).toBeVisible();
    const drawerBox = await drawer.boundingBox();
    expect(drawerBox.y).toBe(47);
    // The drawer runs to the bottom edge; its content stays above the inset.
    expect(drawerBox.y + drawerBox.height).toBe(844);
    const footer = await drawer
      .getByText("Connected", { exact: true })
      .boundingBox();
    expect(footer.y + footer.height).toBeLessThanOrEqual(844 - 34);
    expect(
      await drawer.evaluate((element) => {
        const style = getComputedStyle(element, "::backdrop");
        return [style.borderTopWidth, style.borderTopColor];
      }),
    ).toEqual(["47px", "rgb(255, 255, 255)"]);
    await drawer
      .getByRole("button", { name: "Close conversations", exact: true })
      .click();
    await expect(drawer).toHaveCount(0);
  }
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  const settings = page.getByRole("dialog", { name: "Settings", exact: true });
  const geometry = await settings.evaluate((element) => {
    const box = element.getBoundingClientRect();
    const header = element.querySelector("header");
    const close = header.querySelector("button").getBoundingClientRect();
    // Padding excludes the native scrollbar gutter shared with the body.
    const gutter = header.offsetWidth - header.clientWidth;
    return {
      top: box.top,
      bottom: box.bottom,
      insetTop: close.top - box.top,
      insetRight: box.right - close.right - gutter,
    };
  });
  // A full-screen sheet on a phone: from the safe top to the bottom edge
  // (its own surface runs under the home indicator).
  expect(Math.abs(geometry.top - 47)).toBeLessThanOrEqual(1);
  expect(Math.abs(geometry.bottom - 844)).toBeLessThanOrEqual(1);
  expect(Math.abs(geometry.insetTop - geometry.insetRight)).toBeLessThanOrEqual(
    1,
  );
  await settings
    .locator(".settings-nav")
    .getByRole("button", { name: "This browser", exact: true })
    .click();
  await settings.getByLabel("Appearance", { exact: true }).selectOption("dark");
  await expect(page.locator("#app")).toHaveCSS(
    "background-color",
    "rgb(0, 0, 0)",
  );
  expect(
    await settings.evaluate(
      (element) => getComputedStyle(element, "::backdrop").borderTopColor,
    ),
  ).toBe("rgb(0, 0, 0)");
  await page.setViewportSize({ width: 844, height: 390 });
  await page.evaluate(() => {
    const style = document.documentElement.style;
    style.setProperty("--safe-top", "0px");
    style.setProperty("--safe-bottom", "21px");
    style.setProperty("--safe-left", "47px");
    style.setProperty("--safe-right", "47px");
  });
  await expect(page.locator("html")).toHaveCSS("--viewport-height", "390px");
  // A sheet fills the safe area: clear of the notch and home indicator.
  const landscape = await settings.boundingBox();
  expect(landscape.y).toBeGreaterThanOrEqual(0);
  expect(landscape.y + landscape.height).toBeLessThanOrEqual(390 - 21 + 1);
  expect(landscape.x).toBeGreaterThanOrEqual(47 - 1);
  expect(landscape.x + landscape.width).toBeLessThanOrEqual(844 - 47 + 1);
});

test("fresh conversation reload keeps one stable loading state", async ({
  page,
  session,
}) => {
  let releaseCatalogue;
  let releaseSnapshot;
  const catalogueGate = new Promise((resolve) => (releaseCatalogue = resolve));
  const snapshotGate = new Promise((resolve) => (releaseSnapshot = resolve));
  let catalogueRequested;
  let snapshotRequested;
  const sawCatalogue = new Promise((resolve) => (catalogueRequested = resolve));
  const sawSnapshot = new Promise((resolve) => (snapshotRequested = resolve));

  await page.route("**/api/sessions?refresh=1", async (route) => {
    const response = await route.fetch();
    catalogueRequested();
    await catalogueGate;
    await route.fulfill({ response });
  });
  await page.route(`**/api/sessions/${session.id}`, async (route) => {
    const response = await route.fetch();
    snapshotRequested();
    await snapshotGate;
    await route.fulfill({ response });
  });
  try {
    await page.goto(`/#session=${session.id}`);
    await sawCatalogue;
    // One transcript placeholder, in one place, through the catalogue and
    // the snapshot: loading never restarts or moves.
    const loader = page.locator(".conversation .placeholder .message").first();
    const status = page.locator(".conversation [role=status]", {
      hasText: "Loading conversation…",
    });
    await expect(loader).toHaveCount(1);
    await expect(status).toHaveCount(1);
    const first = await loader.boundingBox();

    releaseCatalogue();
    await sawSnapshot;
    await expect(loader).toHaveCount(1);
    await expect(status).toHaveCount(1);
    const second = await loader.boundingBox();
    expect(Math.abs(first.x - second.x)).toBeLessThan(1);
    expect(Math.abs(first.y - second.y)).toBeLessThan(1);
    expect(Math.abs(first.width - second.width)).toBeLessThan(1);

    releaseSnapshot();
    await expect(
      page.getByRole("heading", { name: "What are we working on?" }),
    ).toBeVisible();
  } finally {
    releaseCatalogue();
    releaseSnapshot();
  }
});

test("touch controls remain reachable at phone width", async ({
  browser,
  host: fixture,
  storageState,
  session,
}) => {
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    storageState,
  });
  const page = await context.newPage();
  try {
    await page.goto(`${fixture.origin}/#session=${session.id}`);
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    expect(
      await page.evaluate(() => matchMedia("(pointer: coarse)").matches),
    ).toBe(true);
    for (const name of ["Model and effort", "Approval", "Send"]) {
      const rect = await page
        .getByRole("button", { name, exact: true })
        .boundingBox();
      expect(Math.min(rect.width, rect.height)).toBeGreaterThanOrEqual(44);
      expect(rect.y + rect.height).toBeLessThanOrEqual(844);
    }
    await page
      .getByRole("button", { name: "Model and effort", exact: true })
      .tap();
    const picker = page.getByRole("dialog", {
      name: "Model and effort",
      exact: true,
    });
    await expect(
      picker.getByRole("combobox", { name: "Model", exact: true }),
    ).toBeVisible();
    const box = await picker.boundingBox();
    expect(box.x + box.width).toBeLessThanOrEqual(390);
    await picker.getByRole("button", { name: "Cancel", exact: true }).tap();
    await page.getByRole("button", { name: "Settings", exact: true }).tap();
    await page
      .locator(".settings-nav")
      .getByRole("button", { name: "This browser", exact: true })
      .tap();
    await page.getByLabel("Appearance").selectOption("dark");
    await page
      .getByRole("button", { name: "Close settings", exact: true })
      .tap();
    await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
  } finally {
    await context.close();
  }
});

test("keyboard viewport preserves focus and contains chat, dialogs and editors", async ({
  browser,
  host: fixture,
  storageState,
  session,
}) => {
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
    storageState,
  });
  // Its own phone context, so it collects its own page errors.
  const page = await context.newPage(),
    errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  await withoutServiceWorker(page);
  // Desktop engines cannot open a phone keyboard. Model an independently
  // resized/panned visual viewport; a window resize alone misses this bug.
  const viewport = async (height, top = 0) => {
    await page.evaluate(
      ({ height, top }) => {
        for (const [key, value] of Object.entries({
          height,
          offsetTop: top,
        }))
          Object.defineProperty(visualViewport, key, {
            configurable: true,
            value,
          });
        visualViewport.dispatchEvent(new Event("resize"));
        visualViewport.dispatchEvent(new Event("scroll"));
      },
      { height, top },
    );
    await expect
      .poll(async () =>
        Math.round((await page.locator("#app").boundingBox()).height),
      )
      .toBe(height);
  };
  const contained = async (locator, height, top = 0) => {
    await expect
      .poll(async () => {
        const box = await locator.boundingBox();
        return (
          !!box &&
          box.y >= top - 1 &&
          box.y + box.height <= top + height + 1 &&
          box.x >= -1 &&
          box.x + box.width <= 391
        );
      })
      .toBe(true);
  };
  const input = async (locator, text, expectedFontSize = 16) => {
    await locator.fill(text);
    await viewport(390, 70);
    await expect(locator).toBeFocused();
    expect(
      await locator.evaluate((e) => parseFloat(getComputedStyle(e).fontSize)),
    ).toBeCloseTo(expectedFontSize, 1);
    await contained(locator, 390, 70);
  };
  try {
    await page.goto(`${fixture.origin}/#session=${session.id}`);
    // The viewport never forbids zoom (WCAG 1.4.4); the shell itself
    // keeps its own pan-only touch action.
    expect(
      await page.locator('meta[name="viewport"]').getAttribute("content"),
    ).not.toMatch(/user-scalable=no|maximum-scale/);
    await expect(page.locator("html")).toHaveCSS("touch-action", "pan-x pan-y");
    const prompt = page.getByLabel("Message or guidance");
    await expect(prompt).toBeVisible();
    await page
      .locator("html")
      .evaluate((element) =>
        element.style.setProperty("--safe-bottom-resting", "34px"),
      );
    await expect(page.locator("#app")).toHaveCSS("padding-bottom", "34px");
    // Status above the input; activity and totals have separate rows below.
    expect((await page.locator(".composer").boundingBox()).height).toBeLessThan(
      200,
    );
    const draft = "Keep my draft and focus as the keyboard moves";
    await input(prompt, draft, 16);
    await expect(page.locator("html")).toHaveAttribute("data-keyboard", "");
    await expect(page.locator("#app")).toHaveCSS("padding-bottom", "0px");
    expect(
      await page.evaluate(() => {
        const app = document.getElementById("app").getBoundingClientRect();
        const composer = document
          .querySelector(".composer")
          .getBoundingClientRect();
        return app.bottom - composer.bottom;
      }),
    ).toBeLessThan(2);
    await prompt.blur();
    await expect(page.locator("html")).toHaveAttribute("data-keyboard", "");
    await expect(page.locator("#app")).toHaveCSS("padding-bottom", "0px");
    await viewport(844);
    await expect(page.locator("html[data-keyboard]")).toHaveCount(0);
    await expect(page.locator("#app")).toHaveCSS("padding-bottom", "34px");
    await prompt.focus();
    for (const [height, top] of [
      [330, 110],
      [460, 30],
      [844, 0],
    ]) {
      await viewport(height, top);
      await contained(page.locator(".composer"), height, top);
      await expect(prompt).toHaveValue(draft);
      await expect(prompt).toBeFocused();
      expect(await page.evaluate(() => scrollY)).toBe(0);
    }
    await viewport(430, 60);
    await page
      .getByRole("button", { name: "Model and effort", exact: true })
      .tap();
    const picker = page.getByRole("dialog", {
      name: "Model and effort",
      exact: true,
    });
    await contained(picker, 430, 60);
    // Safari also sends pans without resize events.
    await page.evaluate(() => {
      Object.defineProperty(visualViewport, "offsetTop", {
        configurable: true,
        value: 90,
      });
      visualViewport.dispatchEvent(new Event("scroll"));
    });
    await expect
      .poll(async () =>
        Math.round((await page.locator("#app").boundingBox()).y),
      )
      .toBe(90);
    await contained(picker, 430, 90);
    await picker.getByRole("button", { name: "Cancel", exact: true }).tap();
    await viewport(844);
    await page.getByRole("button", { name: "Settings", exact: true }).tap();
    const settings = page.getByRole("dialog", {
      name: "Settings",
      exact: true,
    });
    await settings
      .locator(".settings-nav")
      .getByRole("button", { name: "This browser", exact: true })
      .tap();
    await settings.getByLabel("Zoom", { exact: true }).fill("50");
    await settings.getByRole("button", { name: "Back", exact: true }).tap();
    await settings
      .locator(".settings-nav")
      .getByRole("button", { name: "All conversations", exact: true })
      .tap();
    await input(settings.getByLabel("Find a setting"), "web", 16);
    await contained(settings, 390, 70);
    await settings.getByRole("button", { name: "Back", exact: true }).tap();
    await settings
      .locator(".settings-nav")
      .getByRole("button", { name: "This browser", exact: true })
      .tap();
    await settings.getByRole("button", { name: /^Zoom .*reset$/ }).tap();
    await settings
      .getByRole("button", { name: "Close settings", exact: true })
      .tap();
    await viewport(844);
    await page
      .getByRole("button", { name: "Open conversations", exact: true })
      .tap();
    const drawer = page.getByRole("dialog", {
      name: "Conversations",
      exact: true,
    });
    await input(drawer.getByRole("searchbox"), "");
    await contained(drawer, 390, 70);
    await drawer.getByRole("button", { name: "Library", exact: true }).tap();
    await viewport(844);
    await page.getByRole("button", { name: "Add memory", exact: true }).tap();
    await input(page.getByLabel("Name", { exact: true }), "keyboard draft");
    await input(
      page.getByLabel("Document content", { exact: true }),
      "Editable above the keyboard.",
    );
    await viewport(844);
    await page
      .getByRole("button", { name: "Open conversations", exact: true })
      .tap();
    await drawer.getByRole("button", { name: /^Scheduled/ }).tap();
    await page.getByRole("button", { name: "New task", exact: true }).tap();
    await input(
      page.getByLabel("Instructions", { exact: true }),
      "Do not schedule this draft.",
    );
    await viewport(844);
    await input(page.getByLabel("Timezone", { exact: true }), "UTC");
    await viewport(844);
    expect(
      await page.evaluate(
        () => document.documentElement.scrollWidth <= innerWidth,
      ),
    ).toBe(true);
    expect(errors).toEqual([]);
  } finally {
    await context.close();
  }
});

test("a long agent state truncates instead of wrapping the phone status line", async ({
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
  const line = page.locator(".composer .status-line");
  await expect(line).toBeVisible();
  const single = (await line.boundingBox()).height;
  // One line with the state truncated, idle and while work runs; the work
  // button and totals sit under the input.
  const fits = async () => {
    await page
      .locator(".composer .activity-caption")
      .evaluate(
        (node) =>
          (node.textContent =
            "Running · Run · cd /home/dev/Software/project && rg -l --no-messages"),
      );
    expect((await line.boundingBox()).height).toBe(single);
    expect(
      await page
        .locator(".composer .activity-caption")
        .evaluate((node) => node.scrollWidth > node.clientWidth),
    ).toBe(true);
  };
  await fits();
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Background activity probe");
  await prompt.press("Enter");
  await expect(
    page.getByRole("button", { name: "Activity", exact: true }),
  ).toBeVisible();
  await fits();
});

test.describe("composer footer", () => {
  test.use({ hasTouch: true });
  for (const width of [320, 390, 1280]) {
    test(`activity and totals remain readable (${width})`, async ({
      page,
      session,
    }, testInfo) => {
      await page.setViewportSize({ width, height: 844 });
      await page.emulateMedia({ colorScheme: "dark" });
      await page.route(`**/api/sessions/${session.id}`, async (route) => {
        const response = await route.fetch();
        const snapshot = await response.json();
        snapshot.state = {
          ...snapshot.state,
          context_tokens: 4600,
          context_window: 1300000,
          statistics: { ...snapshot.state?.statistics, recorded_turns: 12345 },
          usage: {
            ...snapshot.state?.usage,
            cost_reported: true,
            cost: 1234.56,
          },
          activities: [
            { id: 12, kind: "agent", status: "running", label: "Research" },
            { id: 13, kind: "command", status: "running", label: "Build" },
          ],
        };
        await route.fulfill({ response, json: snapshot });
      });
      await page.goto(`/#session=${session.id}`);
      const footer = page.locator(".composer-metrics");
      const context = footer.getByRole("button", {
        name: "Raw context",
        exact: true,
      });
      await expect(context).toBeEnabled();
      await expect(
        footer.getByRole("button", { name: "Activity", exact: true }),
      ).toContainText("1 agent · 1 command");
      await expect(
        context.locator(width < 600 ? ".context-compact" : ".context-full"),
      ).toBeVisible();
      if (width < 600)
        await expect(context.locator(".context-compact")).toHaveText(
          "Context 99% left",
        );
      const geometry = await footer.evaluate((node) => {
        const rect = (target) => {
          const { x, y, width, height, bottom, right } =
            target.getBoundingClientRect();
          return { x, y, width, height, bottom, right };
        };
        return {
          frame: rect(node),
          activity: rect(node.querySelector(".sheet-control")),
          metrics: rect(node.querySelector(".metrics")),
          buttons: [...node.querySelectorAll("button")].map((button) => ({
            ...rect(button),
            clipped: button.scrollWidth > button.clientWidth + 1,
          })),
        };
      });
      if (width < 600)
        expect(geometry.metrics.y).toBeGreaterThanOrEqual(
          geometry.activity.bottom,
        );
      for (const button of geometry.buttons) {
        expect(button.x).toBeGreaterThanOrEqual(geometry.frame.x - 1);
        expect(button.right).toBeLessThanOrEqual(geometry.frame.right + 1);
        expect(button.clipped).toBe(false);
        expect(button.height).toBeGreaterThanOrEqual(32);
      }
      await page.screenshot({ path: testInfo.outputPath("footer.png") });
      await footer.getByRole("button", { name: "Activity", exact: true }).tap();
      await expect(
        page.getByRole("dialog", { name: "Activity", exact: true }),
      ).toBeVisible();
    });
  }
});
