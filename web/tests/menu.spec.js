// The conversation menu is the app's one nested menu: how focus and the
// open state behave around it, by pointer, touch and keyboard.
import { test, expect, online, openMore } from "./fixtures.js";

for (const touch of [false, true]) {
  test.describe(`with ${touch ? "touch" : "mouse"}`, () => {
    test.use({ hasTouch: touch });

    test("pointer navigation stays unringed and keyboard navigation restores its ring", async ({
      page,
      session,
    }) => {
      await page.goto(`/#session=${session.id}`);
      const activate = (target) => (touch ? target.tap() : target.click());
      const trigger = page
        .locator(".conversation-head")
        .getByRole("button", { name: "Conversation menu", exact: true });
      await activate(trigger);
      await activate(page.getByRole("menuitem", { name: "More", exact: true }));
      await activate(page.getByRole("menuitem", { name: /^Detail/ }));
      await activate(page.getByRole("menuitem", { name: "Back to More" }));
      await activate(page.getByRole("menuitem", { name: /^Detail/ }));
      await activate(
        page.getByRole("menuitemradio", { name: "Default", exact: true }),
      );
      await expect(trigger).toBeFocused();
      await expect(trigger).toHaveCSS("outline-style", "none");

      const sheet = page.getByRole("button", {
        name: "Model and effort",
        exact: true,
      });
      await activate(sheet);
      await activate(
        page.getByRole("button", { name: "Close model and effort" }),
      );
      await expect(sheet).toBeFocused();
      await expect(sheet).toHaveCSS("outline-style", "none");

      await trigger.focus();
      await trigger.press("Enter");
      await expect(page.getByRole("menu")).toBeVisible();
      await page.keyboard.press("Escape");
      await expect(trigger).toBeFocused();
      await expect(trigger).toHaveCSS("outline-style", "solid");
    });

    test.describe("menu toggle", () => {
      test.use({ reducedMotion: "no-preference" });

      test("pressing an expanded menu trigger closes it", async ({
        page,
        session,
      }) => {
        // Older engines ignore showPopover's source option. The trigger must
        // still be associated declaratively so its press is not light-dismiss.
        await page.addInitScript(() => {
          const show = HTMLElement.prototype.showPopover;
          HTMLElement.prototype.showPopover = function () {
            return show.call(this);
          };
        });
        await page.goto(`/#session=${session.id}`);
        const trigger = page
          .locator(".conversation-head")
          .getByRole("button", { name: "Conversation menu", exact: true });
        const menu = page.getByRole("menu", { name: "Conversation menu" });
        const activate = () => (touch ? trigger.tap() : trigger.click());
        for (let index = 0; index < 3; index++) {
          await page.emulateMedia({
            reducedMotion: index === 1 ? "reduce" : "no-preference",
          });
          await activate();
          await expect(menu).toBeVisible();
          await activate();
          await expect(menu).toHaveCount(0);
          await expect(trigger).toHaveAttribute("aria-expanded", "false");
        }
      });
    });
  });
}

test("the conversation menu is short and More holds the rest", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const menu = page.getByRole("menu", { name: "Conversation menu" });
  await page
    .locator(".conversation-head")
    .getByRole("button", { name: "Conversation menu" })
    .click();
  await expect(menu.getByRole("menuitem")).toHaveText([
    "Find in conversation",
    "Pin",
    "Rename",
    "Move to…",
    "More",
  ]);
  await openMore(page);
  await expect(menu.getByRole("menuitem")).toHaveText([
    "More",
    "Fork conversation",
    "Statistics",
    "Tools",
    "Export transcript",
    /^Detail/,
    "Compact",
    "Restart",
    "Close conversation",
    "Delete",
  ]);
  await menu.getByRole("menuitem", { name: "Statistics" }).click();
  await expect(
    page.getByRole("dialog", { name: "Conversation statistics" }),
  ).toBeVisible();
  await page.keyboard.press("Escape");
  await page
    .locator(".conversation-head")
    .getByRole("button", { name: "Conversation menu" })
    .click();
  await openMore(page);
  await menu.getByRole("menuitem", { name: "Delete" }).click();
  await expect(
    page.getByRole("dialog", { name: "Delete conversation?" }),
  ).toBeVisible();
});

test("a conversation's row menu has the same top level", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const row = page.locator(".sidebar .session-row", {
    has: page.locator("[aria-current]"),
  });
  await row.hover();
  await row.getByRole("button", { name: "Conversation menu" }).click();
  await expect(
    page.getByRole("menu", { name: "Conversation menu" }).getByRole("menuitem"),
  ).toHaveText(["Pin", "Rename", "Move to…", "More"]);
});
