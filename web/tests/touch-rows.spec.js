// On a touch screen a list row draws its ⋯ only while it is the open one;
// a long press opens any row's menu.
import { test, expect, online } from "./fixtures.js";

const LONG_PRESS_MS = 450;

test.use({
  isMobile: true,
  hasTouch: true,
  viewport: { width: 390, height: 844 },
});
test.beforeEach(({ browserName }) =>
  test.skip(browserName !== "chromium", "Chromium touch input"),
);

// Two conversations in the drawer: the open one and another.
async function twoRows(page, command, host, session) {
  const { session: other } = await command("create", { cwd: host.project });
  await command("rename", {
    session_id: other.id,
    generation: other.generation || "",
    title: "Other row",
  });
  await page.clock.install();
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.getByRole("button", { name: "Open conversations" }).click();
  const drawer = page.getByRole("dialog", { name: "Conversations" });
  const row = (title) => drawer.locator(".session-row", { hasText: title });
  const menuOf = (title) =>
    row(title).getByRole("button", { name: "Conversation menu" });
  const target = await row("Other row").locator(".session").boundingBox();
  const at = {
    x: target.x + target.width / 2,
    y: target.y + target.height / 2,
  };
  const cdp = await page.context().newCDPSession(page);
  const touch = (type, point = at) =>
    cdp.send("Input.dispatchTouchEvent", {
      type,
      touchPoints: type === "touchEnd" ? [] : [point],
    });
  return { drawer, row, menuOf, touch, at };
}

test("only the open row draws its ⋯, and every row's stays reachable", async ({
  page,
  command,
  host,
  session,
}) => {
  const { menuOf } = await twoRows(page, command, host, session);
  const open = page.locator(".sidebar .session-row:has([aria-current])");
  await expect(open).toHaveCount(1);
  await expect(open.locator(".action-menu")).toHaveCSS("opacity", "1");
  const hidden = menuOf("Other row");
  await expect(hidden.locator("..")).toHaveCSS("opacity", "0");
  // A key press makes the next focus a keyboard's, which draws the button.
  await page.keyboard.press("Shift");
  await hidden.focus();
  await expect(hidden).toBeFocused();
  await expect(hidden.locator("..")).toHaveCSS("opacity", "1");
});

test("a long press opens the row's menu and releasing does not open the conversation", async ({
  page,
  command,
  host,
  session,
}) => {
  const { touch, row } = await twoRows(page, command, host, session);
  await touch("touchStart");
  await page.clock.runFor(LONG_PRESS_MS + 50);
  const menu = page.getByRole("menu", { name: "Conversation menu" });
  await expect(menu).toHaveCount(0);
  // The row says it is held, so the person knows to lift.
  await expect(row("Other row")).toHaveAttribute("data-held", "");
  await touch("touchEnd");
  await expect(menu).toBeVisible();
  await expect(row("Other row")).not.toHaveAttribute("data-held", "");
  await expect(page).toHaveURL(new RegExp(`#session=${session.id}`));
  await expect(row("Other row").locator("[aria-current]")).toHaveCount(0);
});

test("a touch that moves is a scroll, not a long press", async ({
  page,
  command,
  host,
  session,
}) => {
  const { touch, at } = await twoRows(page, command, host, session);
  await touch("touchStart");
  await touch("touchMove", { x: at.x, y: at.y + 40 });
  await page.clock.runFor(LONG_PRESS_MS * 2);
  await touch("touchEnd");
  await expect(page.getByRole("menu")).toHaveCount(0);
});

test("a short tap opens the conversation and no menu", async ({
  page,
  command,
  host,
  session,
}) => {
  const { row } = await twoRows(page, command, host, session);
  await row("Other row").locator(".session").tap();
  await expect(page).not.toHaveURL(new RegExp(`#session=${session.id}`));
  await expect(page.getByRole("menu")).toHaveCount(0);
});

test("a place's header opens its own menu, and shows no system menu", async ({
  page,
  command,
  host,
  session,
}) => {
  const { drawer, touch } = await twoRows(page, command, host, session);
  const head = drawer.locator(".place-head").first();
  const name = head.locator("h2 button");
  const box = await name.boundingBox();
  const at = { x: box.x + box.width / 2, y: box.y + box.height / 2 };
  const prevented = await name.evaluate((node) => {
    const event = new MouseEvent("contextmenu", {
      bubbles: true,
      cancelable: true,
    });
    node.dispatchEvent(event);
    return event.defaultPrevented;
  });
  expect(prevented).toBe(true);
  await touch("touchStart", at);
  await page.clock.runFor(LONG_PRESS_MS + 50);
  await touch("touchEnd");
  await expect(
    page.getByRole("menu", { name: /^Folder menu for/ }),
  ).toBeVisible();
  await expect(name).not.toHaveAttribute("aria-current", "page");
});
