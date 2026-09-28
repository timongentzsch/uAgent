import { test, expect } from "./fixtures.js";
import { mkdir, writeFile } from "node:fs/promises";

test("reading a paragraph survives offsetting changes inside one message", async ({
  page,
  host,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await mkdir(`${host.home}/.uagent/history`, { recursive: true });
  const messages = Array.from({ length: 46 }, (_, index) => ({
    role: index % 2 ? "assistant" : "user",
    content:
      index === 24
        ? Array.from(
            { length: 30 },
            (_, paragraph) =>
              `Anchor paragraph ${paragraph}. ` + "Reading text. ".repeat(9),
          ).join("\n\n")
        : `History message ${index}. ` + "Ordinary text. ".repeat(12),
  }));
  await writeFile(
    `${host.home}/.uagent/history/anchor.json`,
    JSON.stringify({
      format: 3,
      cwd: host.project,
      model: "test",
      session_id: "paragraph-anchor",
      title: "Paragraph anchor",
      turns: 23,
    }) +
      "\n" +
      JSON.stringify({
        messages,
        message_kinds: messages.map((message) => message.role),
        archive: [],
        archive_dropped_segments: 0,
        context_tokens: 0,
        usage: {},
        tool_displays: {},
      }),
  );
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await page.getByRole("button", { name: /Paragraph anchor/ }).click();
  const box = page.locator(".transcript");
  const row = box
    .locator(".message")
    .filter({ hasText: "Anchor paragraph 15." });
  const marker = row.locator("[data-anchor-id]").nth(15);
  await expect(marker).toBeVisible();
  await marker.evaluate((node) => {
    const scroller = node.closest(".transcript");
    scroller.scrollTop +=
      node.getBoundingClientRect().top -
      scroller.getBoundingClientRect().top -
      20;
  });
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeVisible();
  const before = await marker.boundingBox();
  await row.evaluate((node) => {
    const blocks = node.querySelectorAll("[data-anchor-id]");
    blocks[24].style.paddingTop = "120px";
  });
  await expect
    .poll(async () => {
      const next = await marker.boundingBox();
      return Math.abs(next.y - before.y);
    })
    .toBeLessThan(1);
  await row.evaluate((node) => {
    const blocks = node.querySelectorAll("[data-anchor-id]");
    blocks[5].style.paddingTop = "120px";
    blocks[24].style.paddingTop = "0px";
  });
  await expect
    .poll(async () => {
      const next = await marker.boundingBox();
      return Math.abs(next.y - before.y);
    })
    .toBeLessThan(1);
  await page.reload();
  await expect(marker).toBeVisible();
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeVisible();
  await expect
    .poll(async () => {
      const next = await marker.boundingBox();
      return Math.abs(next.y - before.y);
    })
    .toBeLessThan(2);

  // A real wheel event may coincide with a layout change. The reader's
  // movement must survive the anchor compensation in both modes, including
  // on WebKit, which cancels a wheel scroll when scrollTop is written early.
  // Engines scroll different distances per notch, so a plain wheel first
  // measures this one's; the coinciding wheel must move most of that.
  const bounds = await box.boundingBox();
  await page.mouse.move(
    bounds.x + bounds.width / 2,
    bounds.y + bounds.height / 2,
  );
  const settled = async () => {
    let last = (await marker.boundingBox()).y;
    await expect
      .poll(async () => {
        const next = (await marker.boundingBox()).y;
        const still = Math.abs(next - last) < 0.5;
        last = next;
        return still;
      })
      .toBe(true);
    return last;
  };
  const beforePlain = await settled();
  await page.mouse.wheel(0, -120);
  // Animated engines start moving after the event; wait for it, then rest.
  await expect
    .poll(async () => (await marker.boundingBox()).y - beforePlain)
    .toBeGreaterThan(5);
  const notch = (await settled()) - beforePlain;
  const beforeWheel = await marker.boundingBox();
  await box.evaluate((element) => {
    element.addEventListener(
      "wheel",
      () => {
        // A block above the one being read, in the same message.
        const row = [...element.querySelectorAll(".message")].find((node) =>
          node.textContent.includes("Anchor paragraph 15."),
        );
        row.querySelectorAll("[data-anchor-id]")[5].style.paddingTop = "200px";
      },
      { capture: true, once: true },
    );
  });
  await page.mouse.wheel(0, -120);
  await expect
    .poll(async () => (await marker.boundingBox()).y - beforeWheel.y)
    .toBeGreaterThan(notch * 0.75);
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeVisible();

  await page.getByRole("button", { name: "Jump to latest" }).click();
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeHidden();
  await box.evaluate((element) => {
    element.addEventListener(
      "wheel",
      () => {
        element.querySelector(".message [data-anchor-id]").style.paddingTop =
          "100px";
      },
      { capture: true, once: true },
    );
  });
  await page.mouse.wheel(0, -120);
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toBeVisible();
});
