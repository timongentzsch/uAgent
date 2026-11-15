// docs/images/terminal.gif: one conversation in the terminal and on a phone.
// A request typed in the terminal is read, edited and tested by a scripted
// model (tests/web_host.py, FILM); the edit and the command are approved on
// the phone.
import { writeFileSync } from "node:fs";
import {
  test,
  expect,
  headerAction,
  online,
  writeFiles,
} from "../tests/fixtures.js";

const desk = { width: 1200, height: 800 };
const phone = { width: 360, height: 640 };
test.use({ viewport: desk, video: { mode: "on", size: desk } });

const pause = (page, ms = 900) => page.waitForTimeout(ms);

test("terminal", async ({ page, session, host, browser }, testInfo) => {
  test.setTimeout(180000);
  await writeFiles(host.project, {
    "src/inventory.cc": [
      '#include "inventory.h"',
      "",
      "namespace stockroom {",
      "",
      "bool Inventory::Reserve(ItemId item, int count) {",
      "  // TODO: hold the stock for this order.",
      "  return true;",
      "}",
      "",
      "}  // namespace stockroom",
      "",
    ].join("\n"),
    Makefile:
      "test:\n\t@printf 'reserve_holds_stock ... ok\\nreserve_refuses_short ... ok\\nrelease_returns_stock ... ok\\n3 tests passed\\n'\n",
  });
  const began = Date.now();
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await headerAction(page, "Terminal");
  const dock = page.getByRole("region", { name: "Terminal" });
  const shown = dock.locator(".terminal-tabpanel:not([hidden])");
  await expect(shown.locator(".xterm-rows")).toContainText(/\S/);
  // The terminal, as tall as the window allows.
  const edge = await page
    .locator(".terminal-dock > .resize-handle")
    .boundingBox();
  await page.mouse.move(edge.x + edge.width / 2, edge.y + edge.height / 2);
  await page.mouse.down();
  await page.mouse.move(edge.x + edge.width / 2, 60, { steps: 6 });
  await page.mouse.up();
  await shown.locator(".xterm").click();
  // Out of the film: a plain prompt, the fixture's model, the built binary.
  await page.keyboard.type(
    `PS1='$ '; export UAGENT_MODEL=mock/main:high; alias uagent=${process.env.UAGENT_TEST_BINARY}; clear\n`,
  );
  await pause(page, 700);

  // The phone: the same device's pairing, in a window of its own.
  const pocket = await browser.newContext({
    viewport: phone,
    colorScheme: "dark",
    hasTouch: true,
    isMobile: true,
    storageState: await page.context().storageState(),
    recordVideo: { dir: testInfo.outputPath("phone"), size: phone },
  });
  const pocketBegan = Date.now();
  const hand = await pocket.newPage();
  await hand.goto(new URL("/", page.url()).href);
  await pause(hand, 1200);

  const start = Date.now();
  await page.keyboard.type("uagent", { delay: 110 });
  await page.keyboard.press("Enter");
  await expect(shown.locator(".xterm-rows")).toContainText("Ready");
  await pause(page, 900);
  await page.keyboard.type("Reserve stock when an order is placed", {
    delay: 55,
  });
  await pause(page, 500);
  await page.keyboard.press("Enter");

  // On the phone the conversation appears, and asks.
  const row = hand.getByRole("button", { name: /Reserve stock/ }).first();
  await row.tap();
  for (let asked = 0; asked < 2; asked++) {
    const allow = hand.getByRole("button", {
      name: "Allow once",
      exact: true,
    });
    await allow.waitFor({ timeout: 30000 });
    await pause(hand, 1600);
    await allow.tap();
    await pause(hand, 600);
  }
  await expect(shown.locator(".xterm-rows")).toContainText("three tests pass", {
    timeout: 30000,
  });
  await pause(page, 2600);
  const end = Date.now();

  const box = await shown.boundingBox();
  const round = (value) => 2 * Math.round(value / 2);
  const handVideo = hand.video();
  await pocket.close();
  writeFileSync(
    testInfo.outputPath("film.json"),
    JSON.stringify({
      name: "terminal",
      seconds: (end - start) / 1000,
      height: 600,
      parts: [
        {
          // Where the window's film is kept once the test has ended.
          path: "video.webm",
          start: (start - began) / 1000,
          crop: [
            round(box.width),
            round(box.height),
            round(box.x),
            round(box.y),
          ],
        },
        {
          path: await handVideo.path(),
          start: (start - pocketBegan) / 1000,
          crop: [phone.width, phone.height, 0, 0],
        },
      ],
    }),
  );
});
