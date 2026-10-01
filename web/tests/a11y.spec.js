// WCAG 2.2 AA, checked by axe-core on the surfaces a person meets: the
// showcase of every primitive, a live conversation, both kinds of pending
// decision, the settings sheet and the phone drawer. axe is a test-only
// dependency, injected into the page; the product bundle never carries it.
import { test, showcaseTest, expect, SHOWCASE_URL } from "./fixtures.js";
import { createRequire } from "node:module";
import { readdir, readFile } from "node:fs/promises";
import { join } from "node:path";

const AXE = createRequire(import.meta.url).resolve("axe-core/axe.min.js");
const TAGS = ["wcag2a", "wcag2aa", "wcag21aa", "wcag22aa"];

// The violations axe finds in `scope` (a selector; the whole page without
// one), each with the first few nodes it names.
async function violations(page, scope) {
  await page.addScriptTag({ path: AXE });
  const found = await page.evaluate(
    async ({ scope, tags }) => {
      const { violations } = await globalThis.axe.run(
        scope ? document.querySelector(scope) : document,
        { runOnly: { type: "tag", values: tags }, resultTypes: ["violations"] },
      );
      return violations.map((violation) => ({
        id: violation.id,
        help: violation.help,
        nodes: violation.nodes
          .slice(0, 5)
          .map((node) => `${node.target.join(" ")}: ${node.failureSummary}`),
      }));
    },
    { scope, tags: TAGS },
  );
  return found;
}

// The mock provider answers as model-b.
const answeringModel = (command, session) =>
  command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });

// The host's content security policy forbids injected scripts.
test.use({ bypassCSP: true });
showcaseTest.use({ bypassCSP: true });

for (const scheme of ["dark", "light"])
  showcaseTest(
    `the showcase has no violations in ${scheme}`,
    async ({ page }) => {
      await page.emulateMedia({ colorScheme: scheme });
      await page.goto(SHOWCASE_URL);
      await expect(
        page.getByRole("heading", { name: "µAgent UI showcase", exact: true }),
      ).toBeVisible();
      expect(await violations(page)).toEqual([]);
    },
  );

showcaseTest("an ask decision has no violations", async ({ page }) => {
  await page.goto(SHOWCASE_URL);
  const ask = page.getByRole("region", { name: "Pending decision" });
  await ask.scrollIntoViewIfNeeded();
  await expect(ask.getByRole("button", { name: "Submit" })).toBeVisible();
  expect(await violations(page, ".decision")).toEqual([]);
});

showcaseTest("a menu opens, moves and closes by keyboard", async ({ page }) => {
  await page.goto(SHOWCASE_URL);
  const trigger = page.getByRole("button", { name: "Example menu" });
  await trigger.focus();
  const menu = page.getByRole("menu", { name: "Example menu" });
  const item = (name) => menu.getByRole("menuitem", { name });
  // Up opens at the last available item, Down at the first.
  await page.keyboard.press("ArrowUp");
  await expect(item("Second action")).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(trigger).toBeFocused();
  await page.keyboard.press("ArrowDown");
  await expect(item("First action")).toBeFocused();
  // Items leave the tab order; a first letter moves to its item.
  await expect(item("First action")).toHaveAttribute("tabindex", "-1");
  await page.keyboard.press("s");
  await expect(item("Second action")).toBeFocused();
  await page.keyboard.press("f");
  await expect(item("First action")).toBeFocused();
  // Tab leaves from the button, on to what follows it.
  await page.keyboard.press("Tab");
  await expect(menu).toHaveCount(0);
  await expect(
    page.getByRole("button", { name: "Selected state" }),
  ).toBeFocused();
});

showcaseTest(
  "a forced palette still draws the switch knob apart from its track",
  async ({ page }) => {
    await page.emulateMedia({ forcedColors: "active" });
    await page.goto(SHOWCASE_URL);
    const switches = page.locator('.switch input[role="switch"]');
    await expect(switches.first()).toBeVisible();
    for (const index of [0, (await switches.count()) - 1]) {
      const { knob, track } = await switches.nth(index).evaluate((input) => ({
        knob: getComputedStyle(input, "::before").backgroundColor,
        track: getComputedStyle(input).backgroundColor,
      }));
      expect(knob).not.toBe("rgba(0, 0, 0, 0)");
      expect(knob).not.toBe(track);
    }
  },
);

showcaseTest.describe("touch", () => {
  showcaseTest.use({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
  });

  showcaseTest(
    "the remote screen zooms and scrolls by buttons as well as gestures",
    async ({ page }) => {
      await page.goto(SHOWCASE_URL);
      await page.getByRole("button", { name: "Open browser input" }).click();
      const viewport = page.getByLabel("Browser viewport");
      const view = viewport.getByRole("group", { name: "View" });
      await expect(view).toBeVisible();
      const rfb = viewport.locator(".browser-rfb");
      const width = async () => (await rfb.boundingBox()).width;
      const before = await width();
      await view.getByRole("button", { name: "Zoom in" }).click();
      await expect.poll(width).toBeGreaterThan(before);
      await view.getByRole("button", { name: "Zoom out" }).click();
      await expect.poll(width).toBeCloseTo(before, 0);
      await page.evaluate(() => (globalThis.browserPointer ?? []).splice(0));
      await view.getByRole("button", { name: "Scroll down" }).click();
      // Three wheel-down notches (button 5, mask 16), each pressed and
      // released.
      expect(
        await page.evaluate(() =>
          (globalThis.browserPointer ?? []).splice(0).map(([, , mask]) => mask),
        ),
      ).toEqual([16, 0, 16, 0, 16, 0]);
    },
  );
});

test("a conversation with a reply has no violations", async ({
  page,
  session,
  command,
}) => {
  await answeringModel(command, session);
  await page.goto(`/#session=${session.id}`);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Hello there");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.locator(".message.response").first()).toBeVisible();
  await expect(page.locator(".composer .activity-caption")).toHaveText(
    "Ready",
    { timeout: 15000 },
  );
  expect(await violations(page)).toEqual([]);
});

test("an approval decision has no violations and takes focus", async ({
  page,
  session,
  command,
}) => {
  await answeringModel(command, session);
  await page.goto(`/#session=${session.id}`);
  await page.getByLabel("Message or guidance").fill("request approval");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  const heading = page.getByRole("heading", { name: "Needs your decision" });
  await expect(heading).toBeFocused();
  await expect(page.getByRole("alert")).toContainText("Needs your decision");
  expect(await violations(page)).toEqual([]);
  await page.getByRole("button", { name: "Allow once", exact: true }).click();
  await expect(heading).toHaveCount(0);
});

test("the settings sheet has no violations", async ({ page, session }) => {
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  const settings = page.getByRole("dialog", { name: "Settings" });
  await expect(settings).toBeVisible();
  await expect(
    settings.getByRole("status", { name: "Loading settings…" }),
  ).toHaveCount(0);
  expect(await violations(page)).toEqual([]);
});

for (const width of [390, 1280])
  test(`a setting's sheet has no violations at ${width}px, and logging out asks first`, async ({
    page,
    session,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    await page.goto(`/#session=${session.id}`);
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    const settings = page.getByRole("dialog", { name: "Settings" });
    const nav = settings.locator(".settings-nav");
    await nav.getByRole("button", { name: "Models", exact: true }).click();
    expect(await violations(page)).toEqual([]);
    await settings.getByRole("button", { name: /^Sub-agent model/ }).click();
    const sheet = page.getByRole("dialog", { name: "Sub-agent model" });
    await expect(sheet).toContainText("Follows Conversation model");
    expect(await violations(page)).toEqual([]);
    await page.keyboard.press("Escape");
    const back = settings.getByRole("button", { name: "Back", exact: true });
    if (await back.isVisible()) await back.click();
    await nav.getByRole("button", { name: "Devices", exact: true }).click();
    await settings.getByRole("button", { name: "Log out this device" }).click();
    const confirm = page.getByRole("dialog", { name: "Log out" });
    await expect(confirm).toContainText("unpaired");
    expect(await violations(page)).toEqual([]);
    await confirm.getByRole("button", { name: "Cancel" }).click();
    await expect(settings).toBeVisible();
  });

test.describe("phone", () => {
  test.use({ viewport: { width: 390, height: 844 } });

  test("the sessions drawer has no violations", async ({ page, session }) => {
    await page.goto(`/#session=${session.id}`);
    await page.getByRole("button", { name: "Open sessions" }).click();
    const drawer = page.getByRole("dialog", { name: "Sessions" });
    await expect(
      drawer.getByRole("navigation", { name: "Conversations" }),
    ).toBeVisible();
    expect(await violations(page)).toEqual([]);
  });
});

test("the skip link moves focus past the sessions to the conversation", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await expect(page.getByLabel("Message or guidance")).toBeVisible();
  await page.evaluate(() => document.activeElement?.blur());
  await page.keyboard.press("Tab");
  const skip = page.getByRole("link", { name: "Skip to conversation" });
  await expect(skip).toBeFocused();
  await expect(skip).toBeInViewport();
  await page.keyboard.press("Enter");
  await expect(page.getByRole("main")).toBeFocused();
  expect(new URL(page.url()).hash).toBe(`#session=${session.id}`);
});

showcaseTest("the product bundle does not ship axe", async () => {
  const dist = new URL("../dist/", import.meta.url).pathname;
  const files = (await readdir(dist, { recursive: true })).filter((name) =>
    /\.(?:js|html|css)$/.test(name),
  );
  expect(files.length).toBeGreaterThan(0);
  for (const name of files)
    expect(await readFile(join(dist, name), "utf8"), name).not.toContain(
      "axe-core",
    );
});
