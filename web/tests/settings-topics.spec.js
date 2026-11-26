// Settings by topic: navigation, the one scope switch, folded rows, the one
// search, addresses and the ways in.
import { test, expect, online } from "./fixtures.js";
import { offGutter } from "./gutter.js";

const TOPIC_IDS = [
  "conversation",
  "models",
  "approvals",
  "tools",
  "instructions",
  "device",
  "host",
  "about",
];

const dialog = (page) => page.getByRole("dialog", { name: "Settings" });
const topic = (page, name) =>
  dialog(page)
    .getByRole("navigation", { name: "Settings topics" })
    .getByRole("button", { name: new RegExp(`^${name}`) });

test("topics list the open conversation first, and a folder's switch changes where a row is saved", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  const nav = dialog(page).getByRole("navigation", { name: "Settings topics" });
  await expect(nav.getByRole("button")).toHaveText([
    /^This conversation/,
    "Models and providers",
    "Approvals and safety",
    "Tools and extensions",
    "Instructions and memory",
    "This device",
    "Host and devices",
    "About and diagnostics",
  ]);
  await expect(topic(page, "This conversation")).toHaveAttribute(
    "aria-current",
    "page",
  );
  await expect(page).toHaveURL(/settings=conversation$/);

  await topic(page, "Host and devices").click();
  await expect(page).toHaveURL(/settings=host$/);
  const pane = dialog(page).locator(".settings-pane");
  const scope = pane.getByRole("group", { name: "Where it applies" });
  const all = scope.getByRole("button", { name: "All conversations" });
  const folder = scope.getByRole("button", { name: "This folder" });
  await expect(all).toHaveAttribute("aria-pressed", "true");
  // The web host's own settings are never a folder's.
  await expect(pane.getByRole("switch", { name: "Terminal" })).toBeVisible();
  await expect(pane).toContainText("Every conversation on this host");
  await folder.click();
  await expect(folder).toHaveAttribute("aria-pressed", "true");
  // The folder is named under the switch, which keeps two equal halves.
  await expect(pane).toContainText("Conversations in project");
  const [one, two] = await scope.getByRole("button").all();
  expect(
    Math.abs((await one.boundingBox()).width - (await two.boundingBox()).width),
  ).toBeLessThanOrEqual(1);
  await expect(pane.getByRole("switch", { name: "Terminal" })).toHaveCount(0);
  await expect(
    pane.getByRole("region", { name: "Paired devices" }),
  ).toBeVisible();
  await all.click();
  await expect(pane.getByRole("switch", { name: "Terminal" })).toBeVisible();
});

test("Show all reveals the rarely changed rows, and search finds them folded", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await topic(page, "Models and providers").click();
  const pane = dialog(page).locator(".settings-pane");
  const length = pane.locator('[data-row="maxOutputTokens"]');
  await expect(pane.locator('[data-row="model"]')).toBeVisible();
  await expect(length).toHaveCount(0);
  await pane.getByRole("button", { name: /^Show all/ }).click();
  await expect(length).toBeVisible();
  await pane.getByRole("button", { name: /^Show fewer/ }).click();
  await expect(length).toHaveCount(0);

  await dialog(page).getByLabel("Find a setting").fill("response length");
  await dialog(page)
    .getByRole("button", { name: /^Response length/ })
    .click();
  await expect(length).toBeInViewport();
  await expect(page).toHaveURL(/settings=models\/maxOutputTokens$/);
});

test("one search over every topic finds Log level and opens its row", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await dialog(page).getByLabel("Find a setting").fill("log");
  const found = dialog(page).getByRole("region", { name: "Found" });
  const level = found.getByRole("button", { name: /^Log level/ });
  await expect(level).toContainText("About and diagnostics");
  await expect(
    found.getByRole("button", { name: /^Paired devices/ }),
  ).toContainText("Host and devices");
  await level.click();
  await expect(topic(page, "About and diagnostics")).toHaveAttribute(
    "aria-current",
    "page",
  );
  await expect(
    dialog(page).getByRole("combobox", { name: "Log level" }),
  ).toBeInViewport();
  await expect(page).toHaveURL(/settings=about\/logs\.level$/);

  // ⌘K lists the same rows.
  await page.keyboard.press("Escape");
  await page.keyboard.press("ControlOrMeta+K");
  const search = page.getByRole("dialog", { name: "Search" });
  await search.getByRole("combobox").fill("log level");
  await search.getByRole("option", { name: /^Log level/ }).click();
  await expect(
    dialog(page).getByRole("combobox", { name: "Log level" }),
  ).toBeInViewport();
});

test("an address opens Settings there on load, and Back closes it", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.goto(`/#session=${session.id}&settings=models`);
  await page.reload();
  await expect(topic(page, "Models and providers")).toHaveAttribute(
    "aria-current",
    "page",
  );
  await page.goBack();
  await expect(dialog(page)).toHaveCount(0);
  await expect(page).not.toHaveURL(/settings=/);
  await expect(page).toHaveURL(new RegExp(`session=${session.id}`));
});

test("⌘, opens Settings and the status line opens This conversation at Context", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.keyboard.press("ControlOrMeta+Comma");
  await expect(dialog(page)).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(dialog(page)).toHaveCount(0);

  await page
    .getByRole("button", { name: "Context and usage", exact: true })
    .click();
  await expect(topic(page, "This conversation")).toHaveAttribute(
    "aria-current",
    "page",
  );
  await expect(
    dialog(page).getByRole("button", { name: "Raw context" }),
  ).toBeInViewport();
  await expect(page).toHaveURL(/settings=conversation\/context$/);
});

test.describe("on a phone", () => {
  test.use({ viewport: { width: 390, height: 844 }, hasTouch: true });
  test("the header menu reaches Settings, then a topic, then its rows", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    await online(page);
    await page
      .locator(".conversation-head")
      .getByRole("button", { name: "Conversation menu" })
      .tap();
    await page.getByRole("menuitem", { name: "Settings", exact: true }).tap();
    await expect(
      dialog(page).getByRole("heading", { name: "Settings" }),
    ).toBeVisible();
    await topic(page, "Approvals and safety").tap();
    await expect(
      dialog(page).getByRole("heading", { name: "Approvals and safety" }),
    ).toBeVisible();
    const pane = dialog(page).locator(".settings-pane");
    await expect(
      pane.getByRole("combobox", { name: "Approval" }),
    ).toBeVisible();
    const scope = pane.getByRole("group", { name: "Where it applies" });
    const box = await scope.boundingBox();
    expect(box.x).toBeGreaterThanOrEqual(16);
    expect(box.x + box.width).toBeLessThanOrEqual(390 - 16);
    await pane.getByRole("button", { name: /^Show all/ }).tap();
    await expect(pane.locator('[data-row="limits.maxSteps"]')).toBeVisible();
    await dialog(page).getByRole("button", { name: "Back" }).tap();
    await expect(topic(page, "Approvals and safety")).toBeVisible();
  });
});

// The sheet's header on a notched phone: Back, the title and Close on one
// line, the title cut in its own column, each control at the control size,
// and the top inset taken once.
const header = (page) =>
  page.evaluate(() => {
    const found = [];
    const head = document.querySelector("dialog[open] > header");
    const title = head.querySelector("h2");
    const size = (name) => {
      const probe = document.createElement("div");
      probe.style.height = `var(${name})`;
      document.body.append(probe);
      const height = probe.getBoundingClientRect().height;
      probe.remove();
      return height;
    };
    const control = size("--control");
    const top = size("--safe-top");
    const box = head.getBoundingClientRect();
    if (box.top < top - 1 || box.top > top + 1)
      found.push(`the header starts at ${box.top}, the safe area at ${top}`);
    const line = title.getBoundingClientRect();
    for (const button of head.querySelectorAll("button")) {
      const at = button.getBoundingClientRect();
      if (at.height < control - 1)
        found.push(
          `${button.textContent || button.ariaLabel} is ${at.height}px`,
        );
      if (at.top > line.bottom || at.bottom < line.top)
        found.push(
          `${button.textContent || button.ariaLabel} is off the title's line`,
        );
      if (at.left < line.right && at.right > line.left)
        found.push(
          `${button.textContent || button.ariaLabel} overlaps the title`,
        );
    }
    const style = getComputedStyle(title);
    if (
      title.scrollWidth > title.clientWidth + 1 &&
      style.textOverflow !== "ellipsis"
    )
      found.push("the title runs past its column");
    return found;
  });

// Every topic on a phone, every kind of row unfolded: one left and one right
// edge, touch-sized controls, nothing under the home indicator.
for (const [width, height] of [
  [360, 800],
  [390, 844],
  [430, 932],
])
  test.describe(`at ${width} px`, () => {
    test.use({ viewport: { width, height }, hasTouch: true });
    for (const open of [true, false])
      test(`every topic keeps the gutter ${open ? "with" : "without"} a conversation`, async ({
        page,
        session,
      }) => {
        test.setTimeout(120000);
        const at = (topic) =>
          `/#${open ? `session=${session.id}&` : ""}settings=${topic}`;
        await page.goto(at("models"));
        if (open) await online(page);
        for (const id of open
          ? TOPIC_IDS
          : TOPIC_IDS.filter((id) => id !== "conversation")) {
          await page.goto(at(id));
          await page.reload();
          await page.evaluate(() => {
            const style = document.documentElement.style;
            style.setProperty("--safe-top", "47px");
            style.setProperty("--safe-bottom", "34px");
          });
          const pane = dialog(page).locator(".settings-pane");
          await expect(pane.locator(".group").first()).toBeVisible();
          const all = pane.getByRole("button", { name: /^Show all/ });
          if (await all.count()) await all.click();
          await expect.poll(() => offGutter(page), { message: id }).toEqual([]);
          expect(await header(page), id).toEqual([]);
        }
        // The search results page: the same edges.
        await page.goto(at(""));
        await page.reload();
        await dialog(page).getByLabel("Find a setting").fill("e");
        await expect
          .poll(() => offGutter(page, ".settings-nav"), { message: "search" })
          .toEqual([]);
      });
  });
