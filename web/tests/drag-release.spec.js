// @webkit
// A pointer pressed on a page and let go over the app round it: the page
// does not hear that release where it runs in a process of its own, as
// Chrome runs a sandboxed page, nor where a release goes to what is under
// it. What is on the board is pressed by where it is on the board.
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { test, expect, openFiles, viewerOf, writeFiles } from "./fixtures.js";

test.use({
  launchOptions: [
    ({ browserName }, use) =>
      use(
        browserName === "chromium"
          ? {
              args: [
                "--site-per-process",
                "--enable-features=IsolateSandboxedIframes",
              ],
            }
          : {},
      ),
    { scope: "worker" },
  ],
});

const PATH = "boards/flow/board.html";
const BOARD = `<!doctype html>
<meta charset="utf-8">
<meta name="artboard" content="1200x800">
<meta name="board">
<title>Flow</title>
<div id="ad" data-shape="pill" data-at="60 180" data-size="160 60">Ad click</div>
<div id="db" data-shape="database" data-at="400 500" data-size="160 100">Leads</div>
<hr data-from="ad" data-to="db">
`;

test("a drag let go outside the board ends there: no frame is left, and the item put down does not follow the pointer", async ({
  page: app,
  session,
  host,
}) => {
  await writeFiles(host.project, { [PATH]: BOARD });
  const files = await openFiles(app, session, "boards/flow/");
  const { stage } = viewerOf(files);
  await expect(stage.locator("#db")).toBeVisible();
  await files.getByRole("button", { name: "Edit" }).click();
  await expect(files.getByRole("group", { name: "Insert" })).toBeVisible();
  const frame = files.locator(".page-stage iframe:not([inert])");
  // A point of the 1200 by 800 board, where it is in the window.
  const at = async (x, y) => {
    const box = await frame.boundingBox();
    return [box.x + (x * box.width) / 1200, box.y + (y * box.height) / 800];
  };
  const picked = stage.locator("[data-uagent-picked]");
  // Off the board: over the conversation beside the pane.
  const pane = await files.boundingBox();
  const outside = [pane.x - 80, pane.y + 200];
  await app.mouse.move(...(await at(1000, 700)));
  await app.mouse.down();
  await app.mouse.move(...(await at(1100, 760)), { steps: 3 });
  await expect(stage.locator(".uagent-frame")).toHaveCount(1);
  await app.mouse.move(...outside, { steps: 4 });
  await app.mouse.up();
  await expect(stage.locator(".uagent-frame")).toHaveCount(0);
  // The next press is a press of its own: it lets go of what the frame took.
  await app.mouse.click(...(await at(1150, 780)));
  await expect(picked).toHaveCount(0);
  await app.mouse.click(...(await at(480, 550)));
  await expect(picked).toHaveAttribute("id", "db");

  // An item taken off the board is put down where the drag had it.
  await app.mouse.move(...(await at(480, 550)));
  await app.mouse.down();
  await app.mouse.move(...(await at(560, 550)), { steps: 3 });
  await app.mouse.move(...outside, { steps: 4 });
  await app.mouse.up();
  const place = () =>
    stage.locator("#db").evaluate((item) => [item.offsetLeft, item.offsetTop]);
  const put = await place();
  expect(put).not.toEqual([400, 500]);
  await expect
    .poll(() => readFile(join(host.project, PATH), "utf8"))
    .toContain(`id="db" data-shape="database" data-at="${put.join(" ")}"`);
  await app.mouse.move(...(await at(600, 700)), { steps: 3 });
  expect(await place()).toEqual(put);
});
