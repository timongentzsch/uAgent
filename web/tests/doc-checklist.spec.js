// A document's checklist is ticked in Files without Edit: the page draws
// each item's box and the tick is written to the file.
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { test, expect, openFiles, viewerOf, writeFiles } from "./fixtures.js";

test("a checklist's box ticks and unticks its item in the file, outside Edit", async ({
  page: app,
  session,
  host,
}) => {
  const path = "docs/launch.html";
  await writeFiles(host.project, {
    [path]:
      '<!doctype html>\n<meta name="print" content="flow a4">\n<title>Launch</title>\n' +
      '<h1>Launch</h1>\n<ul class="checklist"><li>Book the hall</li><li data-done>Pay</li></ul>\n',
  });
  const files = await openFiles(app, session, path);
  const { stage } = viewerOf(files);
  const saved = () => readFile(join(host.project, path), "utf8");
  const item = stage.getByText("Book the hall");
  await item.click({ position: { x: 4, y: 8 } });
  await expect.poll(saved).toContain('<li data-done="">Book the hall</li>');
  await stage.getByText("Pay").click({ position: { x: 4, y: 8 } });
  await expect.poll(saved).toContain("<li>Pay</li>");
  // A press on the item's words is no tick: the next tick finds it done.
  await item.click({ position: { x: 120, y: 8 } });
  await stage.getByText("Pay").click({ position: { x: 4, y: 8 } });
  await expect.poll(saved).toContain('<li data-done="">Pay</li>');
  expect(await saved()).toContain('<li data-done="">Book the hall</li>');
});
