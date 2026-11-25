import {
  devServerTest as test,
  expect,
  DEV_SERVER_URL,
  fakeStream,
} from "./fixtures.js";

// A tap into a field focuses it without scrolling, so a phone's keyboard
// cannot pan the page, top bar and all; the app's own resize brings the
// field up. A drag, a long press and a tap into a field already focused stay
// the browser's.
const EPOCH = "tap-epoch";
const SESSION = {
  id: "a".repeat(32),
  generation: "g",
  title: "Taps",
  cwd: "/project",
  status: "idle",
  presence: "active",
  updated: Date.now(),
  turns: 1,
};

async function open(page) {
  await page.route("**/api/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    const json = (body) => route.fulfill({ json: { v: 2, ...body } });
    if (path === "/api/sessions")
      return json({
        epoch: EPOCH,
        cursor: 1,
        sessions: [SESSION],
        capabilities: {},
        devices: [],
        commands: [],
      });
    if (path === `/api/sessions/${SESSION.id}`)
      return json({
        epoch: EPOCH,
        cursor: 1,
        metadata: SESSION,
        state: { phase: "idle", view: { nodes: [] } },
        pending: null,
      });
    return route.fulfill({ status: 404, json: { v: 2, error: "no" } });
  });
  await page.addInitScript(fakeStream, { epoch: EPOCH, cursor: 1 });
  await page.addInitScript(() => {
    // No worker here: its failed registration's banner covers the page.
    Object.defineProperty(navigator, "serviceWorker", {
      value: {
        register: () => new Promise(() => {}),
        getRegistration: async () => undefined,
        ready: new Promise(() => {}),
        controller: null,
        addEventListener() {},
        removeEventListener() {},
      },
    });
    // Whether the last touchend's default was prevented, seen after ours.
    addEventListener("touchend", (event) => {
      globalThis.tapTaken = event.defaultPrevented;
    });
  });
  await page.goto(new URL(`/#session=${SESSION.id}`, DEV_SERVER_URL).href);
  await expect(page.getByLabel("Message or guidance")).toBeEnabled();
}

// A touch from one point to another, as the page receives it. WebKit builds
// no Touch, so the events are plain ones carrying the lists the page reads.
const touch = (page, selector, from, to = from, held = 0) =>
  page.evaluate(
    async ({ selector, from, to, held }) => {
      const target = document.querySelector(selector);
      const box = target.getBoundingClientRect();
      const at = ([x, y]) => ({
        identifier: 1,
        target,
        clientX: box.left + x,
        clientY: box.top + y,
      });
      const send = (type, point, down) => {
        const event = new Event(type, { bubbles: true, cancelable: true });
        Object.defineProperty(event, "touches", {
          value: down ? [at(point)] : [],
        });
        Object.defineProperty(event, "changedTouches", {
          value: [at(point)],
        });
        target.dispatchEvent(event);
      };
      send("touchstart", from, true);
      if (to !== from) send("touchmove", to, true);
      if (held) {
        const until = performance.now() + held;
        while (performance.now() < until)
          await new Promise((r) => requestAnimationFrame(r));
      }
      send("touchend", to, false);
      return globalThis.tapTaken;
    },
    { selector, from, to, held },
  );

test.use({
  isMobile: true,
  hasTouch: true,
  viewport: { width: 390, height: 844 },
});

test("a tap into a field focuses it without moving the page or the bar", async ({
  page,
}) => {
  await open(page);
  const prompt = page.getByLabel("Message or guidance");
  const bar = page.locator(".conversation-head");
  const top = await bar.evaluate((node) => node.getBoundingClientRect().top);
  expect(await touch(page, "#prompt", [20, 10])).toBe(true);
  await expect(prompt).toBeFocused();
  // The caret after what is typed.
  expect(
    await prompt.evaluate((node) => [node.selectionStart, node.value.length]),
  ).toEqual([0, 0]);
  // The keyboard rises as the browser reports it, a few steps at a time:
  // the bar stays where it is in every painted frame.
  const tops = await page.evaluate(async () => {
    const bar = document.querySelector(".conversation-head");
    const seen = [];
    for (const height of [844, 760, 640, 520, 460]) {
      Object.defineProperty(visualViewport, "height", {
        configurable: true,
        value: height,
      });
      visualViewport.dispatchEvent(new Event("resize"));
      for (let frame = 0; frame < 3; frame++) {
        await new Promise((r) => requestAnimationFrame(r));
        seen.push(bar.getBoundingClientRect().top);
      }
    }
    return [...new Set(seen)];
  });
  expect(tops).toEqual([top]);
  await expect(page.locator("html[data-keyboard]")).toHaveCount(1);
  expect(await page.evaluate(() => [scrollX, scrollY])).toEqual([0, 0]);
  // The field is above the keyboard, in the app's own room.
  const bottom = await prompt.evaluate(
    (node) => node.getBoundingClientRect().bottom,
  );
  expect(bottom).toBeLessThanOrEqual(460);
  // Tapped again, focused already: the browser places the caret.
  expect(await touch(page, "#prompt", [20, 10])).toBe(false);
});

test("a drag or a long press ending on a field stays the browser's", async ({
  page,
}) => {
  await open(page);
  const prompt = page.getByLabel("Message or guidance");
  expect(await touch(page, "#prompt", [20, 10], [20, 40])).toBe(false);
  expect(await touch(page, "#prompt", [20, 10], [20, 10], 600)).toBe(false);
  await expect(prompt).not.toBeFocused();
});

test("a click with a mouse is untouched", async ({ browser }) => {
  const context = await browser.newContext({
    viewport: { width: 1280, height: 800 },
  });
  const page = await context.newPage();
  await open(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Typed with a keyboard");
  await prompt.blur();
  await prompt.click({ position: { x: 10, y: 10 } });
  await expect(prompt).toBeFocused();
  // The caret where the click put it, not moved to the end.
  expect(await prompt.evaluate((node) => node.selectionStart)).toBeLessThan(
    "Typed with a keyboard".length,
  );
  await context.close();
});

// What a tap opens a chooser or a picker on stays the browser's: the file
// input over the attach button, a colour and a time input.
test("a tap on the attach button opens the file chooser", async ({ page }) => {
  await open(page);
  const chooser = page.waitForEvent("filechooser");
  await page.locator('.composer input[type="file"]').tap();
  await chooser;
});

test("a tap on a text field's label is taken over like one on the field", async ({
  page,
}) => {
  await open(page);
  await page.evaluate(() => {
    const label = document.createElement("label");
    label.id = "named";
    label.style.cssText = "position:fixed;top:200px;left:20px;";
    label.innerHTML = '<span>Name</span> <input type="text" id="name">';
    document.body.append(label);
  });
  expect(await touch(page, "#named span", [4, 4])).toBe(true);
  expect(await page.evaluate(() => document.activeElement?.id)).toBe("name");
});

test("a button inside a text field's label keeps its tap", async ({ page }) => {
  await open(page);
  await page.evaluate(() => {
    const label = document.createElement("label");
    label.style.cssText = "position:fixed;top:200px;left:20px;";
    label.innerHTML =
      '<input type="text" id="secret"> <button type="button" id="show">Show</button>';
    document.body.append(label);
    globalThis.shown = 0;
    document.getElementById("show").addEventListener("click", () => shown++);
  });
  await page.locator("#show").tap();
  expect(await page.evaluate(() => globalThis.shown)).toBe(1);
  expect(await page.evaluate(() => document.activeElement?.id)).not.toBe(
    "secret",
  );
});

test("a tap on a colour or a time input is not taken over", async ({
  page,
}) => {
  await open(page);
  for (const type of ["color", "time", "date", "file"]) {
    await page.evaluate((type) => {
      document.getElementById("picker")?.remove();
      const input = document.createElement("input");
      input.type = type;
      input.id = "picker";
      input.style.cssText = "position:fixed;top:200px;left:20px;";
      document.body.append(input);
    }, type);
    expect(await touch(page, "#picker", [4, 4]), type).toBe(false);
    expect(
      await page.evaluate(() => document.activeElement?.id || ""),
      type,
    ).not.toBe("picker");
  }
});
