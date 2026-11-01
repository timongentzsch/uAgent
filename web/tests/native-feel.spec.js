// What makes the installed app feel native: a press fills at once, switches
// are the platform's, a file goes to the share sheet on a touch device, and
// the screen stays on while a turn runs.
import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import {
  test,
  expect,
  headerAction,
  online,
  selectProbeModel,
} from "./fixtures.js";

// The page believes the primary pointer is a finger.
const touchScreen = () => {
  const original = window.matchMedia.bind(window);
  window.matchMedia = (query) =>
    query === "(pointer: coarse)"
      ? {
          matches: true,
          media: query,
          addEventListener() {},
          removeEventListener() {},
        }
      : original(query);
};

const background = (locator) =>
  locator.evaluate((element) => getComputedStyle(element).backgroundColor);

// The colour a token paints, read the way the page paints it.
const token = (locator, name) =>
  locator.evaluate((element, variable) => {
    const probe = document.createElement("i");
    probe.style.background = `var(${variable})`;
    element.append(probe);
    const color = getComputedStyle(probe).backgroundColor;
    probe.remove();
    return color;
  }, name);

test("a press fills a shared control at once and fades on release", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const quiet = page.locator(".conversation-head .icon-button:enabled").first();
  await expect(quiet).toBeVisible();
  const rest = await background(quiet);
  const box = await quiet.boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await expect
    .poll(() => background(quiet))
    .toBe(await token(quiet, "--hover"));
  // Off the control before release, so the press is not a click.
  await page.mouse.move(0, 0);
  await page.mouse.up();
  await expect.poll(() => background(quiet)).toBe(rest);

  // The primary control has its own colour; the composer's Send is one.
  await page.getByLabel("Message or guidance").fill("a draft");
  const send = page.getByRole("button", { name: "Send", exact: true });
  await expect(send).toBeEnabled();
  const sendBox = await send.boundingBox();
  await page.mouse.move(
    sendBox.x + sendBox.width / 2,
    sendBox.y + sendBox.height / 2,
  );
  await page.mouse.down();
  await expect
    .poll(() => background(send))
    .toBe(await token(send, "--accent-active"));
  await page.mouse.move(0, 0);
  await page.mouse.up();
});

test("a switch is a native switch with a name, a click and a key", async ({
  page,
  session,
}) => {
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
  const memory = settings.getByRole("switch", {
    name: "Memory",
    exact: true,
  });
  await expect(memory).toHaveJSProperty("tagName", "INPUT");
  await expect(memory).toHaveAttribute("switch");
  const before = await memory.isChecked();
  await memory.click();
  await expect(memory).toBeChecked({ checked: !before });
  // Either way the knob sits inside the track, the same gap from each end.
  for (const checked of [true, false]) {
    await memory.setChecked(checked);
    await expect
      .poll(() =>
        memory.evaluate((input, on) => {
          const knob = getComputedStyle(input, "::before");
          const left = parseFloat(knob.left);
          const moved = parseFloat(knob.translate) || 0;
          const end = input.clientWidth - left - parseFloat(knob.width);
          return Math.abs(moved - (on ? end - left : 0));
        }, checked),
      )
      .toBeLessThan(0.5);
  }
  await memory.setChecked(!before);
  await memory.focus();
  await page.keyboard.press("Space");
  await expect(memory).toBeChecked({ checked: before });
});

test.describe("on a touch screen", () => {
  test.beforeEach(async ({ page }) => {
    await page.addInitScript(touchScreen);
  });

  async function openFile(page, session, host) {
    await mkdir(join(host.project, "src"), { recursive: true });
    await writeFile(
      join(host.project, "src", "app.ts"),
      "export const two = 2;\n",
    );
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.goto(`/#session=${session.id}`);
    await headerAction(page, "Files");
    const pane = page.getByRole("region", { name: "Files" });
    const list = pane.getByRole("navigation", { name: "Folder" });
    await list.getByRole("button", { name: "src" }).click();
    await list.getByRole("button", { name: "app.ts" }).click();
    return pane.getByRole("link", { name: "Download app.ts" });
  }

  test("a file goes to the share sheet, downloads where it cannot, and a cancelled sheet is silent", async ({
    page,
    session,
    host,
  }) => {
    await page.addInitScript(() => {
      window.shared = [];
      window.sheet = "share";
      navigator.canShare = (data) =>
        window.sheet !== "none" && !!data?.files?.length;
      navigator.share = async (data) => {
        window.shared.push({
          name: data.files[0].name,
          text: await data.files[0].text(),
        });
        if (window.sheet === "cancel") throw new DOMException("", "AbortError");
      };
    });
    const downloads = [];
    page.on("download", (download) =>
      downloads.push(download.suggestedFilename()),
    );
    const link = await openFile(page, session, host);

    await link.click();
    await expect
      .poll(() => page.evaluate(() => window.shared))
      .toEqual([{ name: "app.ts", text: "export const two = 2;\n" }]);

    // Cancelled: nothing is shown and nothing downloads. The download that
    // follows is the next click's, so none came before it.
    await page.evaluate(() => {
      window.sheet = "cancel";
    });
    await link.click();
    await expect.poll(() => page.evaluate(() => window.shared.length)).toBe(2);
    await page.evaluate(() => {
      window.sheet = "none";
    });
    const next = page.waitForEvent("download");
    await link.click();
    expect((await next).suggestedFilename()).toBe("app.ts");
    expect(downloads).toEqual(["app.ts"]);
    await expect(page.getByRole("alert")).toHaveCount(0);
  });

  test("the screen stays awake while a turn runs and is asked for again on return", async ({
    page,
    session,
    command,
  }) => {
    await page.addInitScript(() => {
      window.locks = { requested: 0, released: 0, current: null };
      Object.defineProperty(navigator, "wakeLock", {
        configurable: true,
        value: {
          request: async () => {
            const lock = { released: false };
            lock.release = async () => {
              lock.released = true;
              window.locks.released++;
            };
            window.locks.requested++;
            window.locks.current = lock;
            return lock;
          },
        },
      });
    });
    await selectProbeModel(command, session);
    await page.goto(`/#session=${session.id}`);
    await online(page);
    expect(await page.evaluate(() => window.locks.requested)).toBe(0);

    // A turn that waits on a decision is still running.
    await command("submit", {
      session_id: session.id,
      generation: session.generation,
      text: "Memory receipt probe: please save this test memory",
    });
    await expect
      .poll(() => page.evaluate(() => window.locks.requested))
      .toBe(1);

    // The page hides: the browser lets go; it is asked for again on return.
    await page.evaluate(() => {
      window.locks.current.released = true;
      let state = "hidden";
      Object.defineProperty(document, "visibilityState", {
        configurable: true,
        get: () => state,
      });
      document.dispatchEvent(new Event("visibilitychange"));
      state = "visible";
      document.dispatchEvent(new Event("visibilitychange"));
    });
    await expect
      .poll(() => page.evaluate(() => window.locks.requested))
      .toBe(2);

    await page.getByRole("button", { name: "Allow once" }).first().click();
    await expect
      .poll(() => page.evaluate(() => window.locks.current.released))
      .toBe(true);
  });
});

test("a desktop does not keep the screen awake", async ({
  page,
  session,
  command,
}) => {
  await page.addInitScript(() => {
    window.requested = 0;
    Object.defineProperty(navigator, "wakeLock", {
      configurable: true,
      value: {
        request: async () => {
          window.requested++;
          return { released: false, release: async () => {} };
        },
      },
    });
  });
  await selectProbeModel(command, session);
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await command("submit", {
    session_id: session.id,
    generation: session.generation,
    text: "Memory receipt probe: please save this test memory",
  });
  await expect(
    page.getByRole("button", { name: "Allow once" }).first(),
  ).toBeVisible();
  expect(await page.evaluate(() => window.requested)).toBe(0);
});
