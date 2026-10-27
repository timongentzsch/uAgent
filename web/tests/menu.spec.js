// The conversation menu is the app's one nested menu: how focus and the
// open state behave around it, by pointer, touch and keyboard.
import { test, expect } from "./fixtures.js";

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
      await activate(page.getByRole("menuitem", { name: /^Detail/ }));
      await activate(
        page.getByRole("menuitem", { name: "Back to Conversation menu" }),
      );
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
      await page.getByRole("menuitem", { name: /^Detail/ }).focus();
      await page.keyboard.press("ArrowRight");
      await expect(page.getByRole("menuitemradio").first()).toBeFocused();
      await page.keyboard.press("Escape");
      await expect(
        page.getByRole("menuitem", { name: /^Detail/ }),
      ).toBeFocused();
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
