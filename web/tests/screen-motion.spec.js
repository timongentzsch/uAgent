// Screens that move: a conversation opened from the drawer slides in, a phone
// sheet is pulled down to close, and the installed app's drawer opens from
// the left edge. Transitions run in WebKit too, the engine iPhones use;
// touches are dispatched through Chromium's input protocol.
import {
  test,
  expect,
  headerAction,
  leaveEnd,
  online,
  saveHistory,
  selectProbeModel,
} from "./fixtures.js";

const phone = { width: 390, height: 844 };

// Counts the view transitions the app starts, each as the direction it set,
// and those that got as far as animating (`ready`).
const spyOnTransitions = () => {
  window.slides = [];
  window.animated = [];
  const original = document.startViewTransition?.bind(document);
  if (original)
    document.startViewTransition = (update) => {
      const direction = document.documentElement.dataset.slide;
      window.slides.push(direction);
      const run = original(update);
      run.ready.then(
        () => window.animated.push(direction),
        () => {},
      );
      return run;
    };
};

const messages = (id) =>
  Array.from({ length: 60 }, (_, index) => ({
    role: index % 2 ? "assistant" : "user",
    content: `${id} message ${index}. ` + "Filler text here. ".repeat(8),
  }));
// Saves conversations by title and returns their ids once the host lists them.
async function seed(host, request, titles) {
  for (const [title, content] of Object.entries(titles))
    await saveHistory(host, `${title}.json`, {
      id: title,
      title,
      turns: 30,
      messages: content || messages(title),
    });
  const { sessions } = await (
    await request.get("/api/sessions?refresh=1")
  ).json();
  return Object.fromEntries(
    Object.keys(titles).map((title) => [
      title,
      sessions.find((item) => item.title === title).id,
    ]),
  );
}

const slides = (page) => page.evaluate(() => window.slides);
const slideOver = (page) =>
  expect(page.locator("html")).not.toHaveAttribute("data-slide", /./);
const title = (page) => page.locator(".conversation-head h1");
const open = async (page, name) => {
  await page.getByRole("button", { name: "Open conversations" }).click();
  await page.getByRole("button", { name: new RegExp(name) }).click();
};
const atEnd = (box) =>
  expect
    .poll(() =>
      box.evaluate(
        (node) => node.scrollHeight - node.clientHeight - node.scrollTop,
      ),
    )
    .toBeLessThan(3);

test.describe("a conversation slides in", () => {
  test.use({ viewport: phone, reducedMotion: "no-preference" });
  let ids;
  test.beforeEach(async ({ page, host, request }) => {
    await page.addInitScript(spyOnTransitions);
    ids = await seed(host, request, {
      "Motion A": 0,
      "Motion B": 0,
      "Motion C": 0,
    });
  });

  test("opening and going back slide, and end where a plain switch does", async ({
    page,
  }) => {
    await page.goto(`/#session=${ids["Motion A"]}`);
    await online(page);
    test.skip(
      await page.evaluate(() => !document.startViewTransition),
      "no view transitions here",
    );
    const box = page.locator(".transcript");
    await leaveEnd(box);
    await open(page, "Motion B");
    await expect(title(page)).toContainText("Motion B");
    await slideOver(page);
    expect(await slides(page)).toEqual(["forward"]);
    await expect
      .poll(() => page.evaluate(() => window.animated))
      .toEqual(["forward"]);
    await expect(page).toHaveURL(new RegExp(`#session=${ids["Motion B"]}`));
    await expect(page.locator(".dialog.sidebar, dialog.sidebar")).toHaveCount(
      0,
    );
    await atEnd(box);
    // Nothing took the focus into the transcript.
    expect(
      await page.evaluate(() => document.activeElement?.closest(".transcript")),
    ).toBeNull();

    await page.goBack();
    await expect(title(page)).toContainText("Motion A");
    await slideOver(page);
    expect(await slides(page)).toEqual(["forward", "back"]);
    // The reading place comes back, unfollowed.
    await expect(
      page.getByRole("button", { name: "Jump to latest" }),
    ).toBeVisible();
    await expect
      .poll(() => box.evaluate((node) => node.scrollTop))
      .toBeLessThan(124);
    await page.getByRole("button", { name: "Open conversations" }).click();
    await expect(
      page
        .getByRole("dialog", { name: "Conversations" })
        .locator("[aria-current]"),
    ).toContainText("Motion A");
  });

  test("two quick choices end on the second", async ({ page }) => {
    await page.goto(`/#session=${ids["Motion A"]}`);
    await online(page);
    await page.getByRole("button", { name: "Open conversations" }).click();
    await page.evaluate(() => {
      for (const name of ["Motion B", "Motion C"])
        [...document.querySelectorAll("dialog.sidebar .session")]
          .find((row) => row.textContent.includes(name))
          .click();
    });
    await expect(title(page)).toContainText("Motion C");
    await slideOver(page);
    await expect(page).toHaveURL(new RegExp(`#session=${ids["Motion C"]}`));
    await expect(page.locator("dialog.sidebar")).toHaveCount(0);
  });

  test("an answer streaming meanwhile shows in full", async ({
    page,
    command,
    session,
  }) => {
    await selectProbeModel(command, session);
    await command("rename", {
      session_id: session.id,
      generation: session.generation || "",
      title: "Motion S",
    });
    await page.goto(`/#session=${ids["Motion A"]}`);
    await online(page);
    await command("submit", {
      session_id: session.id,
      generation: session.generation,
      text: "Long continuity probe",
    });
    await open(page, "Motion S");
    await expect(title(page)).toContainText("Motion S");
    const answer = page.locator(".message.response > .markdown");
    await expect(answer.last()).toContainText(
      "unfinished-looking content retained safely",
      { timeout: 20_000 },
    );
    await slideOver(page);
    const text = (await answer.textContent()) || "";
    expect(text.split("Long retained body sentence.").length - 1).toBe(360);
  });

  test("a transition the browser drew is left alone", async ({ page }) => {
    await page.addInitScript(() =>
      Object.defineProperty(PopStateEvent.prototype, "hasUAVisualTransition", {
        get: () => true,
      }),
    );
    await page.goto(`/#session=${ids["Motion A"]}`);
    await online(page);
    await open(page, "Motion B");
    await expect(title(page)).toContainText("Motion B");
    await slideOver(page);
    await page.goBack();
    await expect(title(page)).toContainText("Motion A");
    expect(await slides(page)).toEqual(["forward"]);
  });

  test("with the list beside the conversation nothing slides", async ({
    page,
  }) => {
    await page.setViewportSize({ width: 1280, height: 900 });
    await page.goto(`/#session=${ids["Motion A"]}`);
    await online(page);
    await page.getByRole("button", { name: /Motion B/ }).click();
    await expect(title(page)).toContainText("Motion B");
    await page.goBack();
    await expect(title(page)).toContainText("Motion A");
    expect(await slides(page)).toEqual([]);
  });

  test.describe("under reduced motion", () => {
    test.use({ reducedMotion: "reduce" });
    test("nothing slides", async ({ page }) => {
      await page.goto(`/#session=${ids["Motion A"]}`);
      await online(page);
      await open(page, "Motion B");
      await expect(title(page)).toContainText("Motion B");
      await page.goBack();
      await expect(title(page)).toContainText("Motion A");
      expect(await slides(page)).toEqual([]);
    });
  });
});

test.describe("touch", () => {
  test.use({ isMobile: true, hasTouch: true, viewport: phone });
  test.beforeEach(({ browserName }) =>
    test.skip(browserName !== "chromium", "Chromium touch input"),
  );

  // Real touches: a finger from one point to another, with `wait` of the
  // page's clock between the moves.
  async function finger(page) {
    const cdp = await page.context().newCDPSession(page);
    const send = (type, point) =>
      cdp.send("Input.dispatchTouchEvent", {
        type,
        touchPoints: type === "touchEnd" ? [] : [point],
      });
    return async (from, to, { wait = 0, steps = 6 } = {}) => {
      await send("touchStart", from);
      for (let step = 1; step <= steps; step++) {
        await send("touchMove", {
          x: from.x + ((to.x - from.x) * step) / steps,
          y: from.y + ((to.y - from.y) * step) / steps,
        });
        if (wait) await page.clock.runFor(wait);
      }
      await send("touchEnd");
    };
  }
  const center = async (locator) => {
    const box = await locator.boundingBox();
    return { x: box.x + box.width / 2, y: box.y + box.height / 2 };
  };
  const history = (page) =>
    page.evaluate(() => ({
      length: window.history.length,
      layer: window.history.state?.layer ?? 0,
    }));

  test.describe("a sheet", () => {
    let sheet, drag;
    test.beforeEach(async ({ page, session }) => {
      await page.clock.install();
      await page.goto(`/#session=${session.id}`);
      await online(page);
      drag = await finger(page);
      await page
        .getByRole("button", { name: "Conversation statistics", exact: true })
        .click();
      sheet = page.getByRole("dialog", { name: "Conversation statistics" });
      await expect(sheet).toBeVisible();
    });

    test("springs back from a short slow pull and closes like Close from a long one", async ({
      page,
    }) => {
      const before = await history(page);
      const grip = await center(
        sheet.getByRole("heading", {
          name: "Conversation statistics",
          exact: true,
        }),
      );
      await drag(grip, { x: grip.x, y: grip.y + 120 }, { wait: 500 });
      await expect(sheet).toBeVisible();
      await expect(sheet).not.toHaveAttribute("data-dragging", /./);
      expect(await history(page)).toEqual(before);

      await drag(grip, { x: grip.x, y: grip.y + 400 });
      await expect(sheet).toHaveCount(0);
      await expect
        .poll(() => history(page))
        .toEqual({
          length: before.length,
          layer: 0,
        });
      const swiped = await history(page);

      await page
        .getByRole("button", { name: "Conversation statistics", exact: true })
        .click();
      await sheet
        .getByRole("button", { name: "Close conversation statistics" })
        .click();
      await expect(sheet).toHaveCount(0);
      await expect.poll(() => history(page)).toEqual(swiped);
    });

    test("a quick flick closes it", async ({ page }) => {
      await page.clock.pauseAt(new Date(Date.now() + 60_000));
      const grip = await center(
        sheet.getByRole("heading", {
          name: "Conversation statistics",
          exact: true,
        }),
      );
      await drag(grip, { x: grip.x, y: grip.y + 60 });
      await expect(sheet).toHaveCount(0);
    });

    test("a body scrolled down scrolls instead", async ({ page }) => {
      const body = sheet.locator(".dialog-body");
      await body.evaluate((node) => {
        const tall = document.createElement("div");
        tall.style.height = "3000px";
        node.append(tall);
        node.style.overflow = "auto";
        node.scrollTop = 200;
      });
      const at = await center(body);
      await drag(at, { x: at.x, y: at.y + 300 });
      await expect(sheet).toBeVisible();
      await expect(sheet).not.toHaveAttribute("data-dragging", /./);
      expect(await body.evaluate((node) => node.scrollTop)).toBeLessThan(200);
    });
  });

  test("the terminal sheet keeps its touches", async ({ page, session }) => {
    await page.clock.install();
    await page.goto(`/#session=${session.id}`);
    await online(page);
    const drag = await finger(page);
    await headerAction(page, "Terminal");
    const sheet = page.getByRole("dialog", { name: "Terminal" });
    await expect(sheet.locator(".xterm-rows")).toContainText(/\S/);
    const top = await sheet.boundingBox();
    await drag(
      { x: top.x + 60, y: top.y + 12 },
      { x: top.x + 60, y: top.y + 600 },
    );
    await expect(sheet).toBeVisible();
    await expect(sheet).not.toHaveAttribute("data-dragging", /./);
  });

  test.describe("the edge of the screen", () => {
    const edge = { x: 6, y: 400 };
    const across = { x: 160, y: 404 };
    const drawer = (page) =>
      page.getByRole("dialog", { name: "Conversations" });
    const standalone = () => {
      const match = matchMedia.bind(window);
      window.matchMedia = (query) =>
        match(query === "(display-mode: standalone)" ? "all" : query);
    };

    test("opens the drawer in the installed app", async ({ page, session }) => {
      await page.addInitScript(standalone);
      await page.goto(`/#session=${session.id}`);
      await online(page);
      await (await finger(page))(edge, across);
      await expect(drawer(page)).toBeVisible();
    });

    test("does nothing in a browser tab", async ({ page, session }) => {
      await page.goto(`/#session=${session.id}`);
      await online(page);
      await (await finger(page))(edge, across);
      await expect(drawer(page)).toHaveCount(0);
    });

    test("does nothing from inside a code block", async ({
      page,
      host,
      request,
    }) => {
      await page.addInitScript(standalone);
      const ids = await seed(host, request, {
        "Motion code": [
          { role: "user", content: "code" },
          { role: "assistant", content: `\`\`\`\n${"x".repeat(400)}\n\`\`\`` },
        ],
      });
      await page.goto(`/#session=${ids["Motion code"]}`);
      await online(page);
      const block = page.locator(".transcript pre").first();
      await expect(block).toBeVisible();
      const inside = { ...(await center(block)), x: 20 };
      expect(
        await page.evaluate(
          ({ x, y }) => !!document.elementFromPoint(x, y)?.closest("pre"),
          inside,
        ),
      ).toBe(true);
      await (await finger(page))(inside, { x: 160, y: inside.y + 4 });
      await expect(drawer(page)).toHaveCount(0);
    });
  });
});
