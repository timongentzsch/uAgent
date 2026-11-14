// The layout follows the room the app has, down to a narrow Split View
// window. Sizes are those a foldable phone may report; no device is named.
import {
  connected,
  test,
  expect,
  headerAction,
  leaveEnd,
  online,
  saveHistory,
} from "./fixtures.js";

const sizes = [
  [313, 890],
  [466, 678],
  [626, 890],
  [890, 626],
  [844, 390],
  [1440, 900],
];

// On a wide window, so the list is there to click.
async function openLong(page, host) {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/");
  await expect(connected(page)).toBeVisible();
  const messages = Array.from({ length: 60 }, (_, index) => ({
    role: index % 2 ? "assistant" : "user",
    content: `Sized message ${index}. ` + "Filler text here. ".repeat(8),
  }));
  await saveHistory(host, "sized.json", {
    id: "sized",
    title: "Sized conversation",
    turns: 30,
    messages,
  });
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await page.getByRole("button", { name: /Sized conversation/ }).click();
  await expect(page.locator(".transcript .message").first()).toBeVisible();
}

// What --sidebar and --chat-min add up to at this zoom, in pixels.
const roomForList = (page) =>
  page.evaluate(() => {
    const probe = document.createElement("div");
    probe.style.width = "calc(var(--sidebar) + var(--chat-min))";
    document.body.append(probe);
    const width = probe.getBoundingClientRect().width;
    probe.remove();
    return width;
  });

// Resizes and waits until the app has drawn the layout it chose.
async function resize(page, width, height) {
  await page.setViewportSize({ width, height });
  const list = width >= (await roomForList(page)) && height > 500;
  await expect(page.locator("aside.sidebar")).toHaveCount(list ? 1 : 0);
  await expect(
    page.getByRole("button", { name: "Open conversations" }),
  ).toHaveCount(list ? 0 : 1);
  return list;
}

async function expectFitting(page, width) {
  const problems = await page.evaluate((width) => {
    const found = [];
    const root = document.documentElement;
    if (root.scrollWidth > root.clientWidth)
      found.push(`document scrolls: ${root.scrollWidth} > ${root.clientWidth}`);
    for (const selector of [
      ".conversation",
      ".conversation-head",
      ".transcript",
      ".composer",
      ".composer > form",
      ".composer-actions",
    ]) {
      const node = document.querySelector(selector);
      if (node.scrollWidth > node.clientWidth + 1)
        found.push(
          `${selector} scrolls: ${node.scrollWidth} > ${node.clientWidth}`,
        );
      const box = node.getBoundingClientRect();
      if (box.left < -0.5 || box.right > width + 0.5)
        found.push(`${selector} leaves the window: ${box.left}..${box.right}`);
    }
    const column = document
      .querySelector(".conversation")
      .getBoundingClientRect();
    for (const node of document.querySelectorAll(
      ".conversation-head button, .composer button, .composer textarea",
    )) {
      const box = node.getBoundingClientRect();
      if (!box.width) continue;
      if (box.left < column.left - 0.5 || box.right > column.right + 0.5)
        found.push(
          `${node.getAttribute("aria-label") || node.className} outside the column`,
        );
    }
    return found;
  }, width);
  expect(problems).toEqual([]);
}

test("every size decides the list by whether it fits and fits the window", async ({
  page,
  host,
}) => {
  await openLong(page, host);
  const widths = Array.from({ length: 18 }, (_, step) => [
    300 + step * 40,
    890,
  ]);
  for (const [width, height] of [...sizes, ...widths]) {
    await resize(page, width, height);
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    await expectFitting(page, width);
  }
});

test("resizing across every threshold keeps the draft, the place and the page", async ({
  page,
  host,
}) => {
  await openLong(page, host);
  await resize(page, 466, 678);
  const input = page.getByLabel("Message or guidance");
  await online(page);
  await input.fill("a draft that stays");
  const box = page.locator(".transcript");
  await leaveEnd(box);
  const mounted = await box.elementHandle();
  const anchor = () =>
    box.evaluate((node) => {
      const view = node.getBoundingClientRect();
      const row = [...node.querySelectorAll(".message")].find(
        (item) => item.getBoundingClientRect().top >= view.top,
      );
      return row.textContent.slice(0, 20);
    });
  const reading = await anchor();
  for (const [width, height] of [
    [626, 890],
    [890, 626],
    [313, 890],
    [466, 678],
  ]) {
    await resize(page, width, height);
    await expect(input).toHaveValue("a draft that stays");
    expect(await mounted.evaluate((node) => node.isConnected)).toBe(true);
    await expect(
      page.getByRole("button", { name: "Jump to latest" }),
    ).toBeVisible();
    await expect
      .poll(() =>
        box.evaluate((node, text) => {
          const view = node.getBoundingClientRect();
          return [...node.querySelectorAll(".message")].some((item) => {
            const rect = item.getBoundingClientRect();
            return (
              item.textContent.startsWith(text) &&
              rect.bottom > view.top &&
              rect.top < view.bottom
            );
          });
        }, reading),
      )
      .toBe(true);
  }
});

test("a following transcript keeps following across a resize", async ({
  page,
  host,
}) => {
  await openLong(page, host);
  await resize(page, 466, 678);
  const box = page.locator(".transcript");
  const atEnd = () =>
    box.evaluate(
      (node) => node.scrollHeight - node.clientHeight - node.scrollTop <= 2,
    );
  await expect.poll(atEnd).toBe(true);
  for (const [width, height] of [
    [890, 626],
    [313, 890],
  ]) {
    await resize(page, width, height);
    await expect.poll(atEnd).toBe(true);
  }
});

test("an open sheet survives a resize", async ({ page, host }) => {
  await openLong(page, host);
  await resize(page, 466, 678);
  await page
    .getByRole("button", { name: "Conversation statistics", exact: true })
    .click();
  const sheet = page.getByRole("dialog", { name: "Conversation statistics" });
  await expect(sheet).toBeVisible();
  for (const [width, height] of [
    [890, 626],
    [313, 890],
    [626, 890],
  ]) {
    await resize(page, width, height);
    await expect(sheet).toBeVisible();
    const box = await sheet.boundingBox();
    expect(box.x).toBeGreaterThanOrEqual(0);
    expect(box.x + box.width).toBeLessThanOrEqual(width + 0.5);
  }
});

test("the terminal refits when the window changes", async ({ page, host }) => {
  await openLong(page, host);
  await headerAction(page, "Terminal");
  for (const [width, height] of [
    [466, 678],
    [890, 626],
    [313, 890],
    [1440, 900],
  ]) {
    await resize(page, width, height);
    const panel = page.locator(".terminal-panel");
    await expect(panel.locator(".xterm-rows")).toContainText(/\S/);
    await expect
      .poll(async () => {
        const view = await panel.boundingBox();
        const screen = await panel.locator(".xterm-screen").boundingBox();
        return (
          screen.x >= view.x - 0.5 &&
          screen.x + screen.width <= view.x + view.width + 0.5 &&
          screen.y + screen.height <= view.y + view.height + 0.5
        );
      })
      .toBe(true);
  }
});

test("a drawer left open when the list fits closes and leaves Back consistent", async ({
  page,
  host,
}) => {
  await openLong(page, host);
  await resize(page, 466, 678);
  const layer = () => page.evaluate(() => history.state?.layer ?? 0);
  expect(await layer()).toBe(0);
  await page.getByRole("button", { name: "Open conversations" }).click();
  await expect(
    page.getByRole("dialog", { name: "Conversations" }),
  ).toBeVisible();
  expect(await layer()).toBe(1);
  await resize(page, 890, 626);
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await expect(page.locator("aside.sidebar")).toBeVisible();
  // The drawer's history entry is gone with it.
  await expect.poll(layer).toBe(0);
  const opened = await page.evaluate(() => location.hash);
  await page.goBack();
  await expect.poll(() => page.evaluate(() => location.hash)).not.toBe(opened);
  await expect(page.locator("aside.sidebar")).toBeVisible();
  await expect(page.getByRole("dialog")).toHaveCount(0);
});

test("the list's room follows the zoom", async ({ page, host }) => {
  await page.addInitScript(() => localStorage.setItem("uagent-zoom", "200"));
  await openLong(page, host);
  const room = Math.round(await roomForList(page));
  expect(room).toBeGreaterThan(1200);
  for (const width of [room - 40, room + 40]) {
    await resize(page, width, 900);
    await expectFitting(page, width);
  }
});

test("side insets keep the list and the header off a side status bar", async ({
  page,
  host,
}) => {
  await openLong(page, host);
  await page.evaluate(() => {
    const style = document.documentElement.style;
    style.setProperty("--safe-left", "47px");
    style.setProperty("--safe-right", "47px");
  });
  for (const [width, height] of [
    [890, 626],
    [466, 678],
  ]) {
    await resize(page, width, height);
    const head = await page.locator(".conversation-head").boundingBox();
    expect(head.x + head.width).toBeLessThanOrEqual(width - 47 + 0.5);
    if (await page.locator("aside.sidebar").count()) {
      expect((await page.locator("aside.sidebar").boundingBox()).x).toBe(47);
      continue;
    }
    // The drawer counts the inset once.
    await page.getByRole("button", { name: "Open conversations" }).click();
    const drawer = page.getByRole("dialog", { name: "Conversations" });
    await expect.poll(async () => (await drawer.boundingBox()).x).toBe(47);
    await page.keyboard.press("Escape");
    await expect(drawer).toHaveCount(0);
  }
});

test("a narrow column's header is as tall with the totals as without", async ({
  page,
  host,
}) => {
  await openLong(page, host);
  const head = page.locator(".conversation-head");
  const height = async () => (await head.boundingBox()).height;
  for (const [width, height_] of [
    [466, 678],
    [700, 800],
  ]) {
    await resize(page, width, height_);
    await expect(page.locator(".head-totals")).toHaveCount(1);
    const top = async () => (await page.locator(".transcript").boundingBox()).y;
    const [withTotals, below] = [await height(), await top()];
    await page.goBack();
    await expect(page.locator(".head-totals")).toHaveCount(0);
    expect(await height()).toBe(withTotals);
    await page.goForward();
    await expect(page.locator(".head-totals")).toHaveCount(1);
    expect(await top()).toBe(below);
  }
});
