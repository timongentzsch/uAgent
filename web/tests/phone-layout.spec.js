import {
  test,
  expect,
  withoutServiceWorker,
  headerAction,
  online,
  selectProbeModel,
  saveHistory,
} from "./fixtures.js";

test.describe("message layout", () => {
  test.use({ hasTouch: true, isMobile: true });

  test("narrow messages use the full width with metadata below the text", async ({
    page,
    host,
    request,
  }, testInfo) => {
    await saveHistory(host, "message-layout.json", {
      id: "message-layout",
      title: "Message layout",
      turns: 1,
      messages: [
        {
          role: "user",
          content:
            "how to serve those models whats the best setup? what are they optimized for",
        },
        {
          role: "assistant",
          content: "Model serving depends on the workload.",
        },
      ],
      display: {
        ids: [1, 2],
        facts: { "m-1": { time: "2026-11-08T10:57:00.000Z" } },
        next: 3,
      },
    });
    await request.get("/api/sessions?refresh=1");
    await page.goto("/");
    // Its row names its place too.
    await page.getByRole("button", { name: /^Message layout / }).click();
    const message = page.locator(".message.user");
    await expect(message).toBeVisible();
    const meta = message.locator(".message-meta");
    const menu = meta.getByRole("button", { name: /^Actions for / });
    for (const width of [390, 320, 600]) {
      await page.setViewportSize({ width, height: 844 });
      // Read both boxes in one frame while the resized viewport settles.
      await expect
        .poll(() =>
          message.evaluate((node) => {
            const body = node
              .querySelector(".message-text")
              .getBoundingClientRect();
            const footer = node
              .querySelector(".message-meta")
              .getBoundingClientRect();
            return (
              footer.y >= body.bottom - 1 &&
              Math.abs(body.width - footer.width) < 1
            );
          }),
        )
        .toBe(true);
      await expect(meta.locator("time")).toBeVisible();
      const target = await menu.boundingBox();
      expect(target.width).toBeGreaterThanOrEqual(44);
      expect(target.height).toBeGreaterThanOrEqual(44);
      expect(
        await message.evaluate((node) => node.scrollWidth <= node.clientWidth),
      ).toBe(true);
      if (width === 390)
        await page.screenshot({
          path: testInfo.outputPath("phone-message.png"),
        });
    }
    await menu.tap();
    await expect(
      page.getByRole("menuitem", { name: "Edit from here", exact: true }),
    ).toBeVisible();
    await page.keyboard.press("Escape");
    await page.setViewportSize({ width: 1440, height: 900 });
    await expect
      .poll(() =>
        message.evaluate((node) => {
          const body = node
            .querySelector(".message-text")
            .getBoundingClientRect();
          const metadata = node
            .querySelector(".message-meta")
            .getBoundingClientRect();
          return Math.abs(body.y - metadata.y) < 1 && metadata.x >= body.right;
        }),
      )
      .toBe(true);
  });
});

test("mobile chrome keeps an opaque safe area and applies appearance before app startup", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.emulateMedia({ colorScheme: "dark" });
  await page.addInitScript(() => {
    localStorage.setItem("uagent-theme", "light");
    localStorage.setItem("uagent-zoom", "50");
  });
  const { promise: startup, resolve: release } = Promise.withResolvers();
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
    await expect(
      page.locator('meta[name="theme-color"][media*="light"]'),
    ).toHaveAttribute("content", "#fff");
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
  await expect(page.getByRole("heading", { name: "µAgent" })).toBeVisible();
  // Desktop WebKit exposes no iPhone system inset: the safe area is set here.
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
    expect(drawerBox.y + drawerBox.height).toBe(844);
    const footer = await drawer
      .locator("footer")
      .getByRole("status")
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
  // iOS scrolls the shell to reveal a focused drawer that runs past it, and
  // the interface stays shifted: nothing may scroll the app's frame.
  expect(
    await page.evaluate(() =>
      ["#app", ".shell"].map((selector) => {
        const frame = document.querySelector(selector);
        const tall = frame.appendChild(document.createElement("div"));
        tall.style.height = "200vh";
        frame.scrollTop = 34;
        const top = frame.scrollTop;
        tall.remove();
        return top;
      }),
    ),
  ).toEqual([0, 0]);
  await headerAction(page, "Settings");
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
  const { promise: catalogueGate, resolve: releaseCatalogue } =
    Promise.withResolvers();
  const { promise: snapshotGate, resolve: releaseSnapshot } =
    Promise.withResolvers();
  const { promise: sawCatalogue, resolve: catalogueRequested } =
    Promise.withResolvers();
  const { promise: sawSnapshot, resolve: snapshotRequested } =
    Promise.withResolvers();

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
    const loader = page.locator(".conversation .placeholder .message").first();
    const status = page.locator(".conversation [role=status]", {
      hasText: "Loading conversation…",
    });
    await expect(loader).toHaveCount(1);
    await expect(status).toHaveCount(1);

    releaseCatalogue();
    await sawSnapshot;
    await expect(loader).toHaveCount(1);
    await expect(status).toHaveCount(1);

    releaseSnapshot();
    await expect(
      page.getByRole("heading", { name: "What are we working on?" }),
    ).toBeVisible();
  } finally {
    releaseCatalogue();
    releaseSnapshot();
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
    // The viewport never forbids zoom (WCAG 1.4.4).
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
    // Only a focused field holds the keyboard: iOS may keep reporting the
    // small viewport after the keyboard closes.
    await prompt.blur();
    await expect(page.locator("html[data-keyboard]")).toHaveCount(0);
    await viewport(844);
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
    await headerAction(page, "Settings");
    const settings = page.getByRole("dialog", {
      name: "Settings",
      exact: true,
    });
    await settings
      .locator(".settings-nav")
      .getByRole("button", { name: "This browser", exact: true })
      .tap();
    await settings.getByLabel("Zoom", { exact: true }).selectOption("50");
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
    await settings.getByLabel("Zoom", { exact: true }).selectOption("100");
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
    await drawer.getByRole("button", { name: "Search", exact: true }).tap();
    const search = page.getByRole("dialog", { name: "Search" });
    await input(search.getByRole("combobox"), "");
    await contained(search, 390, 70);
    await page.keyboard.press("Escape");
    await viewport(844);
    await page
      .getByRole("button", { name: "Open conversations", exact: true })
      .tap();
    await drawer.getByRole("button", { name: "Library", exact: true }).tap();
    await viewport(844);
    await page
      .locator(".management-toolbar")
      .getByRole("button", { name: "New", exact: true })
      .tap();
    await page.getByRole("menuitem", { name: "Memory", exact: true }).tap();
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
            { id: 12, status: "running", label: "Research" },
            { id: 13, status: "running", label: "Build" },
          ],
        };
        await route.fulfill({ response, json: snapshot });
      });
      await page.goto(`/#session=${session.id}`);
      const footer = page.locator(".composer-metrics");
      // A phone has the totals under the title, which they must fit.
      const phone = width < 600;
      const totals = (
        phone ? page.locator(".conversation-head") : footer
      ).getByRole("button", {
        name: "Conversation statistics",
        exact: true,
      });
      await expect(footer.locator(".activity-button")).toHaveText("2 commands");
      await expect(totals).toHaveText("99% left · 12.3k turns · $1234.56");
      const geometry = await footer.evaluate((node) => {
        const rect = (target) => {
          const { x, y, width, height, bottom, right } =
            target.getBoundingClientRect();
          return { x, y, width, height, bottom, right };
        };
        return {
          frame: rect(node),
          activity: rect(node.querySelector(".sheet-control")),
          metrics: node.querySelector(".metrics")
            ? rect(node.querySelector(".metrics"))
            : null,
          buttons: [...node.querySelectorAll("button")].map((button) => ({
            ...rect(button),
            clipped: button.scrollWidth > button.clientWidth + 1,
          })),
        };
      });
      if (phone) {
        expect(geometry.metrics).toBeNull();
        const head = await totals.evaluate((button) => ({
          clipped: button.scrollWidth > button.clientWidth + 1,
          inside:
            button.getBoundingClientRect().right <=
            button.closest(".conversation-head").getBoundingClientRect().right,
        }));
        // The worst totals ellipsize on the narrowest screen, never overflow.
        expect(head.inside).toBe(true);
        if (width > 320) expect(head.clipped).toBe(false);
      }
      for (const button of geometry.buttons) {
        expect(button.x).toBeGreaterThanOrEqual(geometry.frame.x - 1);
        expect(button.right).toBeLessThanOrEqual(geometry.frame.right + 1);
        expect(button.clipped).toBe(false);
        expect(button.height).toBeGreaterThanOrEqual(32);
      }
      await page.screenshot({ path: testInfo.outputPath("footer.png") });
      await footer.locator(".activity-button").tap();
      await expect(
        page.getByRole("dialog", { name: "Activity", exact: true }),
      ).toBeVisible();
    });
  }
});

test.describe("composer rows", () => {
  test.beforeEach(({ command, session }) => selectProbeModel(command, session));

  test("a column that narrows has the narrow layout in that same frame", async ({
    page,
    session,
  }) => {
    await page.setViewportSize({ width: 1280, height: 844 });
    await page.goto(`/#session=${session.id}`);
    await online(page);
    const below = () =>
      page.evaluate(
        () =>
          document.querySelector(".composer-metrics").getBoundingClientRect()
            .height,
      );
    await expect.poll(below).toBeGreaterThan(0);
    const narrowed = await page.evaluate(
      () =>
        new Promise((resolve) => {
          requestAnimationFrame(() => {
            document.querySelector("#app").style.width = "390px";
            // The next task runs once this frame has painted.
            setTimeout(() =>
              resolve(
                document
                  .querySelector(".composer-metrics")
                  .getBoundingClientRect().height,
              ),
            );
          });
        }),
    );
    expect(narrowed).toBe(0);
  });

  test("an idle composer holds no empty row; the input rises by one row while working", async ({
    page,
    session,
  }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto(`/#session=${session.id}`);
    await online(page);
    const prompt = page.getByLabel("Message or guidance");
    const rows = () =>
      page.evaluate(() => {
        const top = (selector) =>
          document.querySelector(selector).getBoundingClientRect();
        const transcript = document.querySelector(".transcript");
        return {
          input: top(".composer > form").bottom,
          status: top(".status-line").height,
          below: top(".composer-metrics").height,
          off:
            transcript.scrollHeight -
            transcript.scrollTop -
            transcript.clientHeight,
        };
      });
    const idle = await rows();
    expect(idle.status).toBe(0);
    expect(idle.below).toBe(0);
    await prompt.fill("Queue controls probe");
    await prompt.press("Enter");
    await expect(page.locator(".composer .status-led.running")).toBeVisible();
    await expect(page.locator(".message.response")).toContainText(
      "Waiting for guidance",
    );
    const working = await rows();
    expect(working.below).toBeGreaterThan(0);
    expect(idle.input - working.input).toBe(working.below);
    await expect.poll(async () => (await rows()).off).toBeLessThanOrEqual(2);
    await prompt.fill("Carry on");
    await page.getByRole("button", { name: "Queue next" }).click();
    await page.getByRole("button", { name: "Insert now" }).click();
    await expect(page.locator(".turn-footer")).toHaveCount(1);
    await expect(page.locator(".composer .status-led.running")).toHaveCount(0);
    await expect.poll(async () => (await rows()).input).toBe(idle.input);
    await expect.poll(async () => (await rows()).off).toBeLessThanOrEqual(2);
  });
});

test.describe("phone header", () => {
  test.use({ hasTouch: true, viewport: { width: 390, height: 844 } });

  test("the totals sit under the title, the menu is short and Settings is in the drawer", async ({
    page,
    session,
    command,
  }) => {
    await command("rename", {
      session_id: session.id,
      generation: session.generation || "",
      title:
        "A title long enough that it cannot fit the width of a phone header",
    });
    await page.goto(`/#session=${session.id}`);
    await online(page);
    const head = page.locator(".conversation-head");
    const totals = head.getByRole("button", {
      name: "Conversation statistics",
      exact: true,
    });
    await expect(totals).toHaveText(/^\d+% left · \d+ turns?$/);
    const geometry = await head.evaluate((node) => {
      const box = (selector) =>
        node.querySelector(selector).getBoundingClientRect();
      const title = node.querySelector("h1");
      return {
        head: node.getBoundingClientRect(),
        title: box("h1"),
        totals: box(".head-totals"),
        ellipsized: title.scrollWidth > title.clientWidth,
      };
    });
    expect(geometry.ellipsized).toBe(true);
    expect(geometry.totals.top).toBeGreaterThanOrEqual(geometry.title.bottom);
    expect(geometry.totals.bottom).toBeLessThanOrEqual(geometry.head.bottom);
    await expect(page.locator(".composer .metrics")).toHaveCount(0);
    expect(
      await page
        .locator(".composer-metrics")
        .evaluate((node) => node.getBoundingClientRect().height),
    ).toBe(0);
    await totals.tap();
    await expect(
      page.getByRole("dialog", { name: "Conversation statistics" }),
    ).toBeVisible();
    await page.keyboard.press("Escape");

    await head.getByRole("button", { name: "Conversation menu" }).tap();
    const items = page
      .getByRole("menu", { name: "Conversation menu" })
      .getByRole("menuitem");
    await expect(items.filter({ hasText: "Settings" })).toHaveCount(0);
    await expect(items.last()).toHaveText("More");
    await page.keyboard.press("Escape");
    await headerAction(page, "Settings");
    await expect(
      page.getByRole("dialog", { name: "Settings", exact: true }),
    ).toBeVisible();
  });
});
