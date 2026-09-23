import { test, expect } from "./fixtures.js";
import { readFile } from "node:fs/promises";

const geometry = (locator) =>
  locator.evaluate((node) => {
    const { x, y, width, height } = node.getBoundingClientRect();
    return { x, y, width, height };
  });

test("connection status distinguishes active retries from an offline stop", async ({
  page,
  session,
}) => {
  await page.addInitScript(() => {
    window.testStreams = [];
    window.EventSource = class extends EventTarget {
      static CONNECTING = 0;
      static OPEN = 1;
      static CLOSED = 2;
      readyState = 0;
      constructor(url) {
        super();
        this.cursor = new URL(url, location.href).searchParams.get("cursor");
        window.testStreams.push(this);
      }
      close() {
        this.readyState = 2;
      }
      ready() {
        this.readyState = 1;
        this.onopen?.(new Event("open"));
        const separator = this.cursor.lastIndexOf(":");
        this.dispatchEvent(
          new MessageEvent("ready", {
            data: JSON.stringify({
              epoch: this.cursor.slice(0, separator),
              cursor: Number(this.cursor.slice(separator + 1)),
            }),
          }),
        );
      }
    };
  });
  await page.goto(`/#session=${session.id}`);
  const status = page.locator(".sidebar .connection");
  await expect(status).toHaveText("Connecting…");
  await expect.poll(() => page.evaluate(() => testStreams.length)).toBe(1);
  await page.evaluate(() => testStreams[0].ready());
  await expect(status).toHaveText("Connected");
  await page.evaluate(() => {
    testStreams[0].readyState = EventSource.CONNECTING;
    testStreams[0].onerror(new Event("error"));
  });
  await expect(status).toHaveText("Reconnecting…");
  await expect(status.locator(".spinner")).toBeVisible();
  // Opening the transport alone must not claim the event stream is caught up.
  await page.evaluate(() => testStreams[0].onopen(new Event("open")));
  await expect(status).toHaveText("Reconnecting…");
  await page.evaluate(() => testStreams[0].ready());
  await expect(status).toHaveText("Connected");
  await page.evaluate(() => dispatchEvent(new Event("offline")));
  await expect(status).toHaveText("Disconnected");
  await expect(status.locator(".spinner")).toHaveCount(0);
  await page.evaluate(() => dispatchEvent(new Event("online")));
  await expect(status).toHaveText("Reconnecting…");
  await expect.poll(() => page.evaluate(() => testStreams.length)).toBe(2);
  await page.evaluate(() => testStreams[1].ready());
  await expect(status).toHaveText("Connected");
});

for (const width of [390, 1280]) {
  test(`statistics at ${width}px share controls through code and data loading`, async ({
    page,
    session,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    const manifest = JSON.parse(
      await readFile(new URL("../dist/.vite/manifest.json", import.meta.url)),
    );
    let releaseCode, releaseData;
    const code = new Promise((resolve) => (releaseCode = resolve));
    const data = new Promise((resolve) => (releaseData = resolve));
    let reachedData;
    const dataRequested = new Promise((resolve) => (reachedData = resolve));
    let holdData = false;
    await page.route(
      `**/${manifest["src/features/settings/statistics.tsx"].file}`,
      async (route) => {
        await code;
        await route.continue();
      },
    );
    await page.route(`**/api/sessions/${session.id}`, async (route) => {
      if (holdData) {
        reachedData();
        await data;
      }
      const response = await route.fetch();
      const snapshot = await response.json();
      snapshot.state.view.blocks = [
        {
          id: "stats-turn",
          kind: "assistant",
          text: "A completed answer.",
          summary: { outcome: "completed", steps: 1, usage: {} },
        },
      ];
      await route.fulfill({ json: snapshot });
    });
    try {
      await page.goto(`/#session=${session.id}`);
      const trigger = page.getByRole("button", {
        name: "Turn statistics",
        exact: true,
      });
      await expect(trigger).toBeVisible();
      holdData = true;
      await trigger.click();
      const dialog = page.getByRole("dialog", {
        name: "Message statistics",
        exact: true,
      });
      const toolbar = dialog.getByRole("group", { name: "Statistics scope" });
      const turn = toolbar.getByRole("button", { name: "Turn", exact: true });
      await expect(turn).toBeDisabled();
      await expect(dialog.getByRole("status")).toHaveText(
        "Loading statistics…",
      );
      const shell = await geometry(dialog);
      const controls = await geometry(toolbar);
      releaseCode();
      await dataRequested;
      await expect(turn).toBeDisabled();
      expect(await geometry(dialog)).toEqual(shell);
      expect(await geometry(toolbar)).toEqual(controls);
      releaseData();
      await expect(turn).toBeEnabled();
      await expect(dialog.locator(".stats").first()).toBeVisible();
      expect(await geometry(dialog)).toEqual(shell);
      expect(await geometry(toolbar)).toEqual(controls);
      await toolbar
        .getByRole("button", { name: "Session", exact: true })
        .click();
      await expect(turn).toHaveAttribute("aria-pressed", "false");
    } finally {
      releaseCode();
      releaseData();
    }
  });
}

test("browser shell keeps its bounds through cold code and data loading", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const manifest = JSON.parse(
    await readFile(new URL("../dist/.vite/manifest.json", import.meta.url)),
  );
  let releaseCode, releaseData;
  const code = new Promise((resolve) => (releaseCode = resolve));
  const data = new Promise((resolve) => (releaseData = resolve));
  await page.route(
    `**/${manifest["src/features/browser/browser.tsx"].file}`,
    async (route) => {
      await code;
      await route.continue();
    },
  );
  let statusReads = 0;
  await page.route("**/api/browser/status", async (route) => {
    if (++statusReads > 1) await data;
    await route.fulfill({
      json: {
        ok: true,
        mode: "idle",
        running: false,
        profiles: [{ id: "default", name: "Default" }],
      },
    });
  });
  try {
    await page.goto(`/#session=${session.id}`);
    const trigger = page.getByRole("button", { name: "Open browser" });
    await trigger.focus();
    await trigger.press("Enter");
    const dialog = page.getByRole("dialog", { name: "Browser", exact: true });
    await expect(dialog.getByRole("status")).toHaveText("Loading browser…");
    const shell = await geometry(dialog);
    releaseCode();
    await expect
      .poll(() => page.locator('link[href*="browser-"]').count())
      .toBeGreaterThan(0);
    expect(await geometry(dialog)).toEqual(shell);
    releaseData();
    await expect(
      dialog.getByRole("button", { name: "Take control" }),
    ).toBeVisible();
    expect(await geometry(dialog)).toEqual(shell);
    await page.keyboard.press("Escape");
    await expect(dialog).toHaveCount(0);
    await expect(trigger).toBeFocused();
  } finally {
    releaseCode();
    releaseData();
  }
});

test("loading showcase keeps a fixed shell and keyboard focus", async ({
  page,
}) => {
  await page.goto("/ui.html");
  const trigger = page.getByRole("button", { name: "Open loading dialog" });
  await trigger.focus();
  await trigger.press("Enter");
  const dialog = page.getByRole("dialog", {
    name: "Loading example",
    exact: true,
  });
  await expect(dialog.getByRole("status")).toBeVisible();
  await expect(
    dialog.getByRole("heading", { name: "Loading example" }),
  ).toBeFocused();
  const close = dialog.getByRole("button", { name: "Close loading example" });
  await expect(close).not.toBeFocused();
  await page.keyboard.press("Tab");
  await expect(close).toBeFocused();
  await expect(close).toHaveCSS("outline-style", "solid");
  const shell = await geometry(dialog);
  await expect(dialog.getByLabel("Loaded value")).toBeVisible();
  expect(await geometry(dialog)).toEqual(shell);
  await page.keyboard.press("Escape");
  await expect(trigger).toBeFocused();
});

test.describe("touch interaction", () => {
  test.use({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
  });
  test("standalone rotation recovers from stale visual viewport dimensions and offsets", async ({
    page,
    session,
  }) => {
    await page.addInitScript(() => {
      const original = window.matchMedia.bind(window);
      window.matchMedia = (query) => {
        const media = original(query);
        if (query === "(display-mode: standalone)")
          Object.defineProperty(media, "matches", { value: true });
        return media;
      };
    });
    await page.goto(`/#session=${session.id}`);
    const stale = async (width, height, left, top) =>
      page.evaluate(
        ({ width, height, left, top }) => {
          for (const [key, value] of Object.entries({
            width,
            height,
            offsetLeft: left,
            offsetTop: top,
          }))
            Object.defineProperty(visualViewport, key, {
              configurable: true,
              value,
            });
          dispatchEvent(new Event("orientationchange"));
          visualViewport.dispatchEvent(new Event("resize"));
        },
        { width, height, left, top },
      );
    const aligned = async (width, height) => {
      await expect
        .poll(() => geometry(page.locator("#app")))
        .toEqual({ x: 0, y: 0, width, height });
      await expect(page.locator("html[data-keyboard]")).toHaveCount(0);
      expect(await page.evaluate(() => scrollX + scrollY)).toBe(0);
    };
    await aligned(390, 844);
    await page.setViewportSize({ width: 844, height: 390 });
    await stale(390, 844, 34, 70);
    await aligned(844, 390);
    await page.setViewportSize({ width: 390, height: 844 });
    await stale(844, 325, 47, 90);
    await aligned(390, 844);
    await page.getByRole("button", { name: "Settings", exact: true }).tap();
    const dialog = page.getByRole("dialog", { name: "Settings", exact: true });
    const bounds = await geometry(dialog);
    expect(bounds.x).toBeGreaterThanOrEqual(0);
    expect(bounds.x + bounds.width).toBeLessThanOrEqual(390);
    expect(bounds.y).toBeGreaterThanOrEqual(0);
    expect(bounds.y + bounds.height).toBeLessThanOrEqual(844);
  });
  test("touch does not acquire desktop hover and small-scale fields keep a readable font", async ({
    page,
  }) => {
    await page.goto("/ui.html");
    expect(
      await page.evaluate(() => matchMedia("(hover: hover)").matches),
    ).toBe(false);
    const button = page.getByRole("button", {
      name: "Secondary action",
      exact: true,
    });
    const color = await button.evaluate(
      (node) => getComputedStyle(node).backgroundColor,
    );
    await button.tap();
    await expect(button).toHaveCSS("background-color", color);
    await page.getByLabel("Zoom", { exact: true }).fill("50");
    for (const field of [
      page.getByRole("textbox", { name: /^Text / }),
      page.getByRole("searchbox", { name: "Search", exact: true }),
      page.getByRole("textbox", { name: "Long text", exact: true }),
    ]) {
      expect(
        await field.evaluate((node) =>
          parseFloat(getComputedStyle(node).fontSize),
        ),
      ).toBeGreaterThanOrEqual(16);
    }
  });
});
