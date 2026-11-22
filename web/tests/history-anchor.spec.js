import { connected, test, expect, saveHistory } from "./fixtures.js";

// A long conversation open at a paragraph in the middle of one message.
async function readParagraph(page, host) {
  await page.goto("/");
  await expect(connected(page)).toBeVisible();
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
  await saveHistory(host, "anchor.json", {
    id: "paragraph-anchor",
    title: "Paragraph anchor",
    turns: 23,
    messages,
  });
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
  return { box, row, marker, before: await marker.boundingBox() };
}

test("reading a paragraph survives offsetting changes inside one message", async ({
  page,
  host,
}) => {
  const { box, row, marker, before } = await readParagraph(page, host);
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

  // WebKit cancels a wheel scroll when scrollTop is written early. Engines
  // scroll different distances per notch, so a plain wheel measures this
  // one's; the wheel that meets a layout change must move most of that.
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

test("a reload keeps the place when the list arrives after the transcript", async ({
  page,
  host,
}) => {
  // A client without service workers: WebKit sends a controlled page's
  // requests past routes, and the list must be held.
  await page.addInitScript(() => delete Navigator.prototype.serviceWorker);
  const { marker, before } = await readParagraph(page, host);
  let release;
  const held = new Promise((resolve) => (release = resolve));
  const list = /\/api\/sessions(\?refresh=1)?$/;
  await page.route(list, async (route) => {
    await held;
    await route.continue();
  });
  await page.reload();
  await expect(marker).toBeVisible();
  await expect
    .poll(async () => Math.abs((await marker.boundingBox()).y - before.y))
    .toBeLessThan(2);
  await expect(connected(page)).toHaveCount(0);
  release();
  await expect(connected(page)).toBeVisible();
  // Two frames: the list's render, and any scroll it caused.
  await page.evaluate(
    () =>
      new Promise((done) =>
        requestAnimationFrame(() => requestAnimationFrame(done)),
      ),
  );
  expect(Math.abs((await marker.boundingBox()).y - before.y)).toBeLessThan(2);
});

// A reload whose first read of the conversation is refused, its place, and a
// place left in what is no longer there.
test.describe("a reload's place", () => {
  // A client without service workers: WebKit sends a controlled page's
  // requests past routes.
  test.beforeEach(({ page }) =>
    page.addInitScript(() => delete Navigator.prototype.serviceWorker),
  );

  test("is kept when the first read of the conversation fails", async ({
    page,
    host,
  }) => {
    const { marker, before } = await readParagraph(page, host);
    const id = new URL(page.url()).hash.match(/session=([^&]+)/)[1];
    let refusing = true;
    await page.route(new RegExp(`/api/sessions/${id}$`), (route) =>
      refusing
        ? route.fulfill({ status: 503, body: "unavailable" })
        : route.continue(),
    );
    await page.reload();
    const retry = page.getByRole("button", { name: "Retry", exact: true });
    await expect(retry).toBeVisible();
    refusing = false;
    await retry.click();
    await expect(marker).toBeVisible();
    await expect
      .poll(async () => Math.abs((await marker.boundingBox()).y - before.y))
      .toBeLessThan(2);
  });

  test("whose message is gone is let go", async ({ page, host }) => {
    await readParagraph(page, host);
    const messages = [{ role: "user", content: "Only this now." }];
    await saveHistory(host, "anchor.json", {
      id: "paragraph-anchor",
      title: "Paragraph anchor",
      turns: 1,
      messages,
    });
    await page.reload();
    // Restored: rows that arrive from here on are live.
    await expect(page.locator(".transcript")).toHaveAttribute("data-live", "");
  });

  test("in an empty conversation is let go", async ({ page, session }) => {
    await page.addInitScript((id) => {
      sessionStorage.setItem(
        "uagent-transcript-bookmarks",
        JSON.stringify({
          [`chat:${id}`]: { follow: false, message: "m-25", inner: "28" },
        }),
      );
    }, session.id);
    await page.goto(`/#session=${session.id}`);
    await expect(page.getByText("What are we working on?")).toBeVisible();
    await expect(page.locator(".transcript")).toHaveAttribute("data-live", "");
  });
});
