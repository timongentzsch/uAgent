// WCAG 2.2 AA, checked by axe-core on the surfaces a person meets: a live
// conversation in both colour schemes, both kinds of pending decision, the
// settings sheet and the phone drawer. axe is a test-only
// dependency, injected into the page; the product bundle never carries it.
import { test as bare } from "@playwright/test";
import {
  test,
  expect,
  headerAction,
  selectProbeModel,
  servePending,
  SAMPLE_ASK,
} from "./fixtures.js";
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

// The host's content security policy forbids injected scripts.
test.use({ bypassCSP: true });

test("an ask decision has no violations", async ({
  page,
  session,
  request,
}) => {
  await servePending(page, request, session, SAMPLE_ASK);
  await page.goto(`/#session=${session.id}`);
  const ask = page.getByRole("region", { name: "Pending decision" });
  await expect(ask.getByRole("button", { name: "Next" })).toBeVisible();
  expect(await violations(page, ".decision")).toEqual([]);
  // The review page: one tap each on the two single-choice questions.
  await ask.getByRole("radio", { name: /^Memory/ }).click();
  await ask.getByRole("checkbox", { name: /^macOS/ }).check();
  await ask.getByRole("button", { name: "Next" }).click();
  await ask.getByRole("radio", { name: /^Tabs/ }).click();
  await expect(ask.getByRole("button", { name: "Submit" })).toBeVisible();
  expect(await violations(page, ".decision")).toEqual([]);
});

test("the conversation menu opens, moves and closes by keyboard", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  const trigger = page
    .locator(".conversation-head")
    .getByRole("button", { name: "Conversation menu" });
  await trigger.focus();
  const menu = page.getByRole("menu", { name: "Conversation menu" });
  const item = (name) => menu.getByRole("menuitem", { name });
  // Up opens at the last available item, Down at the first.
  await page.keyboard.press("ArrowUp");
  await expect(item("Delete")).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(trigger).toBeFocused();
  await page.keyboard.press("ArrowDown");
  await expect(item("Find in conversation")).toBeFocused();
  // Items leave the tab order; a first letter moves to its item.
  await expect(item("Find in conversation")).toHaveAttribute("tabindex", "-1");
  await page.keyboard.press("r");
  await expect(item("Rename")).toBeFocused();
  await page.keyboard.press("d");
  await expect(item(/^Detail/)).toBeFocused();
  // Right opens the row's view in place and moves into it; Left and Escape
  // lead back to the row, and only then does Escape close the menu.
  await page.keyboard.press("ArrowRight");
  await expect(menu.getByRole("group", { name: "Detail" })).toBeVisible();
  await expect(item("Find in conversation")).toHaveCount(0);
  await expect(menu.getByRole("menuitemradio").first()).toBeFocused();
  expect(await violations(page, ".menu-panel")).toEqual([]);
  await page.keyboard.press("ArrowLeft");
  await expect(item(/^Detail/)).toBeFocused();
  await page.keyboard.press("Enter");
  await expect(menu.getByRole("menuitemradio").first()).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(item(/^Detail/)).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(menu).toHaveCount(0);
  await expect(trigger).toBeFocused();
  // Tab leaves from the open menu, on to what follows it.
  await page.keyboard.press("ArrowDown");
  await expect(item("Find in conversation")).toBeFocused();
  await page.keyboard.press("Tab");
  await expect(menu).toHaveCount(0);
});

test("a forced palette still draws the switch knob apart from its track", async ({
  page,
  session,
}) => {
  await page.emulateMedia({ forcedColors: "active" });
  await page.goto(`/#session=${session.id}`);
  await headerAction(page, "Settings");
  const settings = page.getByRole("dialog", { name: "Settings", exact: true });
  await settings
    .locator(".settings-nav")
    .getByRole("button", { name: "All conversations", exact: true })
    .click();
  await settings
    .getByRole("searchbox", { name: "Find a setting" })
    .fill("memory");
  const switches = settings.locator('.switch input[role="switch"]');
  await expect(switches.first()).toBeVisible();
  for (const index of [0, (await switches.count()) - 1]) {
    const { knob, track } = await switches.nth(index).evaluate((input) => ({
      knob: getComputedStyle(input, "::before").backgroundColor,
      track: getComputedStyle(input).backgroundColor,
    }));
    expect(knob).not.toBe("rgba(0, 0, 0, 0)");
    expect(knob).not.toBe(track);
  }
});

for (const scheme of ["dark", "light"])
  test(`a conversation with a reply has no violations in ${scheme}`, async ({
    page,
    session,
    command,
  }) => {
    await page.emulateMedia({ colorScheme: scheme });
    await selectProbeModel(command, session);
    await page.goto(`/#session=${session.id}`);
    const prompt = page.getByLabel("Message or guidance");
    await prompt.fill("Hello there");
    await page.getByRole("button", { name: "Send", exact: true }).click();
    await expect(page.locator(".message.response").first()).toBeVisible();
    await expect(page.locator(".composer .turn-status-caption")).toHaveText(
      "Ready",
    );
    expect(await violations(page)).toEqual([]);
  });

test("an approval decision has no violations and takes focus", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session);
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

for (const scheme of ["dark", "light"])
  test(`the files pane and the resize handles have no violations in ${scheme}`, async ({
    page,
    session,
  }) => {
    await page.emulateMedia({ colorScheme: scheme });
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.goto(`/#session=${session.id}`);
    await headerAction(page, "Files");
    await page.getByRole("button", { name: "shot.png" }).click();
    await expect(
      page.getByRole("button", { name: "View shot.png" }),
    ).toBeVisible();
    await page.getByRole("combobox", { name: "Search files" }).fill("png");
    await expect(page.getByRole("option", { name: /shot\.png/ })).toBeVisible();
    expect(await violations(page)).toEqual([]);
  });

for (const width of [390, 1280])
  test(`the settings form has no violations at ${width}px, and logging out asks first`, async ({
    page,
    session,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    await page.goto(`/#session=${session.id}`);
    await headerAction(page, "Settings");
    const settings = page.getByRole("dialog", { name: "Settings" });
    const nav = settings.locator(".settings-nav");
    await nav
      .getByRole("button", { name: "All conversations", exact: true })
      .click();
    await expect(
      settings.locator('[data-setting="UAGENT_MEMORY_MODEL"]'),
    ).toContainText("follows Conversation model");
    expect(await violations(page)).toEqual([]);
    const back = settings.getByRole("button", { name: "Back", exact: true });
    if (await back.isVisible()) await back.click();
    await nav.getByRole("button", { name: "Host", exact: true }).click();
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
    await page.getByRole("button", { name: "Open conversations" }).click();
    const drawer = page.getByRole("dialog", { name: "Conversations" });
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

bare("the product bundle does not ship axe", async () => {
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
