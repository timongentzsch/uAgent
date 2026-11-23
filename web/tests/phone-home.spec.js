// The home screen on a phone: the question centred over the last three
// conversations as plain lines, the box docked below; while the keyboard is
// up for the box, or the box holds text, only the question and the box.
import { test, expect, savedConversation, unfiled } from "./fixtures.js";
import { roomy } from "./roomy.js";

test.use({ isMobile: true, hasTouch: true });

// The keyboard cannot be shown here: it is modelled as the browser reports
// it, a visual viewport 460px high.
const keyboard = (page, height) =>
  page.evaluate((height) => {
    Object.defineProperty(visualViewport, "height", {
      configurable: true,
      value: height,
    });
    visualViewport.dispatchEvent(new Event("resize"));
  }, height);
for (const size of [
  { width: 390, height: 844 },
  { width: 320, height: 568 },
]) {
  test.describe(`the home screen at ${size.width} wide`, () => {
    test.use({ places: true, viewport: size });

    const home = (page) => ({
      head: page.locator(".home-head h1"),
      box: page.locator(".home-start"),
      field: page.getByLabel("Start a conversation"),
      lists: page.locator(".home-list").filter({ visible: true }),
      rows: page.locator(".home-list.recent li").filter({ visible: true }),
    });
    // The distance of the box's foot from the screen's.
    const dock = async (box) =>
      await box.evaluate(
        (node) => innerHeight - node.getBoundingClientRect().bottom,
      );
    // Each row whole inside the screen above the box, none cut by it.
    const whole = async (page, selector) => {
      const above = await page
        .locator(".home-start")
        .evaluate((node) => node.getBoundingClientRect().top);
      for (const box of await page.locator(selector).evaluateAll((nodes) =>
        nodes
          .filter((node) => node.getBoundingClientRect().height > 0)
          .map((node) => {
            const { top, bottom } = node.getBoundingClientRect();
            return { top, bottom };
          }),
      )) {
        expect(box.top).toBeGreaterThanOrEqual(0);
        expect(box.bottom).toBeLessThanOrEqual(above + 1);
      }
    };
    const talks = async (host, request, count) => {
      for (let at = 1; at <= count; at++)
        await savedConversation(
          host,
          request,
          `Talk ${at} with a title far longer than any phone is wide, by a good margin`,
        );
    };

    test("at rest the question stands in the room above the last three as plain lines over the box", async ({
      page,
      host,
      request,
    }) => {
      await talks(host, request, 5);
      await page.goto("/");
      const { head, box, rows } = home(page);
      await expect(head).toBeVisible();
      await expect(rows).toHaveCount(3);
      // Plain lines: no frame round them, no place line.
      const frame = await page
        .locator(".home-list.recent ul")
        .evaluate((list) => getComputedStyle(list).borderTopWidth);
      expect(frame).toBe("0px");
      await expect(
        page.locator(".home-list.recent .row-detail").filter({ visible: true }),
      ).toHaveCount(0);
      // The question is centred across, and the box is the screen's foot.
      const at = await head.boundingBox();
      expect(Math.abs(at.x + at.width / 2 - size.width / 2)).toBeLessThan(2);
      expect(await dock(box)).toBeLessThan(40);
      await whole(page, ".home-list.recent li");
      // The lists sit right over the box; the question has the room above.
      const listed = await page
        .locator(".home-list.recent li")
        .filter({ visible: true })
        .last()
        .boundingBox();
      const docked = await box.boundingBox();
      expect(docked.y - (listed.y + listed.height)).toBeLessThan(40);
      expect(at.y + at.height).toBeLessThan(listed.y);
      await roomy(page);
    });

    test("with one conversation, or none, nothing is cut", async ({
      page,
      host,
      request,
    }) => {
      await page.goto("/");
      const { head, box } = home(page);
      await expect(head).toBeVisible();
      await expect(page.locator(".home-list")).toHaveCount(0);
      await roomy(page);
      await talks(host, request, 1);
      await page.reload();
      await expect(page.locator(".home-list.recent li")).toHaveCount(1);
      await whole(page, ".home-list.recent li");
      expect(await dock(box)).toBeLessThan(40);
      await roomy(page);
    });

    test("suggested moves wait above the recent ones, and give way as the box is used", async ({
      page,
      host,
      request,
    }) => {
      await unfiled(host, request, "Three days in Lisbon", {
        options: [{ workspace: "Lisbon trip" }],
        reason: "No place you have fits it.",
      });
      await talks(host, request, 3);
      await page.goto("/");
      const { head, box, field, lists } = home(page);
      await expect(lists).toHaveCount(2);
      await expect(
        page.getByRole("region", { name: "Suggested moves" }),
      ).toBeVisible();
      await whole(page, ".home-list li");
      await roomy(page);
      await field.tap();
      await keyboard(page, 460);
      await expect(lists).toHaveCount(0);
      await expect(head).toBeVisible();
      await roomy(page);
      expect(await dock(box)).toBeLessThan(40 + size.height - 460);
    });

    test("only the keyboard or text in the box moves the lists away, and the box stays where it is", async ({
      page,
      host,
      request,
    }) => {
      await talks(host, request, 3);
      await page.goto("/");
      const { head, box, field, lists } = home(page);
      await expect(lists).toHaveCount(1);
      const docked = await dock(box);
      // A focus that raises no keyboard changes nothing.
      await field.tap();
      await expect(page.locator("html[data-keyboard]")).toHaveCount(0);
      await expect(lists).toHaveCount(1);
      expect(await dock(box)).toBe(docked);
      // The keyboard up for the box: the question and the box fit above it.
      await keyboard(page, 460);
      await expect(page.locator("html[data-keyboard]")).toHaveCount(1);
      await expect(lists).toHaveCount(0);
      await expect(head).toBeVisible();
      await whole(page, ".home-head");
      await roomy(page);
      // Down again with the focus kept: the lists are back.
      await keyboard(page, size.height);
      await expect(lists).toHaveCount(1);
      expect(await dock(box)).toBe(docked);
      // Text typed, then the focus left: the lists stay away.
      await field.fill("Look into the build");
      await head.tap();
      await expect(field).not.toBeFocused();
      await expect(lists).toHaveCount(0);
      // Emptied, they return.
      await field.fill("");
      await head.tap();
      await expect(lists).toHaveCount(1);
    });

    test("Home from the list shows the last ones every time, with no field taking the focus", async ({
      page,
      host,
      request,
    }) => {
      await talks(host, request, 3);
      await page.goto("/");
      const { field, rows } = home(page);
      await expect(rows).toHaveCount(3);
      for (let press = 0; press < 2; press++) {
        await page.getByRole("button", { name: "Open conversations" }).tap();
        const drawer = page.getByRole("dialog", { name: "Conversations" });
        await drawer.getByRole("button", { name: "Home" }).tap();
        await expect(drawer).toBeHidden();
        await expect(rows).toHaveCount(3);
        await expect(field).not.toBeFocused();
      }
    });
  });
}
