// The header's Files button shows the conversation's folder beside the
// chat (a sheet on a small screen): a list with search, and the open file as
// highlighted lines, rendered markdown or an image.
import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { test, expect, headerAction } from "./fixtures.js";

async function folder(project) {
  await mkdir(join(project, "src"));
  await writeFile(
    join(project, "src", "app.ts"),
    "// One.\nexport const two = 2;\nexport const three = `3\n4`;\n",
  );
  await writeFile(
    join(project, "README.md"),
    "# Demo\n\n![The shot](shot.png) See [the app](src/app.ts).\n",
  );
  await writeFile(join(project, "blob.bin"), Buffer.from([0, 1, 2, 0, 255]));
}

// Opening or closing the pane narrows the chat in place: the transcript
// keeps its element, its scroll and its history, never loading them again.
test("the files pane does not remount the chat", async ({ page, session }) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const transcript = page.locator(".transcript");
  await expect(transcript).toBeVisible();
  await transcript.evaluate((element) => {
    window.keptTranscript = element;
  });
  const same = () =>
    page.evaluate(
      () => document.querySelector(".transcript") === window.keptTranscript,
    );
  await headerAction(page, "Files");
  await expect(page.getByRole("region", { name: "Files" })).toBeVisible();
  expect(await same()).toBe(true);
  await headerAction(page, "Files");
  await expect(page.getByRole("region", { name: "Files" })).toHaveCount(0);
  expect(await same()).toBe(true);
});

test("files beside the chat: browse, read, search and view", async ({
  page,
  session,
  host,
}) => {
  await folder(host.project);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await headerAction(page, "Files");
  const pane = page.getByRole("region", { name: "Files" });
  const list = pane.getByRole("navigation", { name: "Folder" });
  // Folders first.
  await expect(list.getByRole("button").nth(1)).toHaveText("src");

  await list.getByRole("button", { name: "src" }).click();
  await list.getByRole("button", { name: "app.ts" }).click();
  await expect(pane.getByRole("heading")).toHaveText("src/app.ts");
  // One numbered row per line; the template string spans two.
  await expect(pane.locator(".code-view .line-number")).toHaveCount(4);
  await expect(pane.locator(".code-view .line").nth(1)).toHaveText(
    "export const two = 2;",
  );

  // Markdown is rendered; its image and link resolve in its folder.
  await list.getByRole("button", { name: /^project$/ }).click();
  await list.getByRole("button", { name: "README.md" }).click();
  await expect(pane.locator(".markdown [data-level='1']")).toHaveText("Demo");
  await expect(pane.getByRole("img", { name: "The shot" })).toHaveJSProperty(
    "naturalWidth",
    1,
  );
  await pane.getByRole("button", { name: "Source" }).click();
  await expect(pane.locator(".code-view .line-number")).toHaveCount(3);
  await pane.getByRole("button", { name: "Source" }).click();
  await pane.getByRole("link", { name: "the app" }).click();
  await expect(pane.getByRole("heading")).toHaveText("src/app.ts");

  // An image opens in the viewer; a file that is not text offers a download.
  await list.getByRole("button", { name: /^project$/ }).click();
  await list.getByRole("button", { name: "shot.png" }).click();
  await pane.getByRole("button", { name: "View shot.png" }).click();
  await expect(page.getByRole("dialog", { name: "shot.png" })).toBeVisible();
  await page.keyboard.press("Escape");
  await list.getByRole("button", { name: "blob.bin" }).click();
  await expect(pane.getByText("blob.bin is not text")).toBeVisible();

  // Search reaches into folders; Enter opens the active match.
  const search = pane.getByRole("combobox", { name: "Search files" });
  await search.fill("apts");
  await expect(pane.getByRole("option")).toHaveText(["app.tssrc"]);
  await search.press("Enter");
  await expect(pane.getByRole("heading")).toHaveText("src/app.ts");

  await pane.getByRole("button", { name: "Close files" }).click();
  await expect(pane).toHaveCount(0);
});

test("on a phone files are a sheet showing the list or the file", async ({
  page,
  session,
  host,
}) => {
  await folder(host.project);
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  await headerAction(page, "Files");
  const sheet = page.getByRole("dialog", { name: "Files" });
  const list = sheet.getByRole("navigation", { name: "Folder" });
  await list.getByRole("button", { name: "README.md" }).click();
  await expect(sheet.locator(".markdown [data-level='1']")).toHaveText("Demo");
  await expect(list).toBeHidden();
  await sheet.getByRole("button", { name: "Back to the list" }).click();
  await expect(list).toBeVisible();
  // No pane to resize on a small screen.
  await expect(page.getByRole("separator")).toHaveCount(0);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth - window.innerWidth,
    ),
  ).toBeLessThanOrEqual(0);
});

test("docked panes resize by drag and keys, and keep their width", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  const sidebar = page.getByRole("complementary", {
    name: "Folders and conversations",
  });
  const width = async (locator) => (await locator.boundingBox()).width;
  const before = await width(sidebar);
  const edge = page.getByRole("separator", { name: "Resize the sidebar" });
  await edge.focus();
  await edge.press("ArrowRight");
  await edge.press("Shift+ArrowRight");
  expect(await width(sidebar)).toBeCloseTo(before + 80, 0);
  await page.reload();
  await expect.poll(() => width(sidebar)).toBeCloseTo(before + 80, 0);
  await edge.press("Enter");
  expect(await width(sidebar)).toBeCloseTo(before, 0);

  // The files pane widens when its edge is dragged left, and stops short
  // of crowding out the chat.
  await headerAction(page, "Files");
  const pane = page.locator(".side-pane");
  const handle = page.getByRole("separator", { name: "Resize the files" });
  const start = await width(pane);
  const box = await handle.boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + 100);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width / 2 - 120, box.y + 100, { steps: 4 });
  await page.mouse.up();
  expect(await width(pane)).toBeCloseTo(start + 120, 0);
  await page.mouse.move(box.x - 120, box.y + 100);
  await page.mouse.down();
  await page.mouse.move(0, box.y + 100, { steps: 4 });
  await page.mouse.up();
  const chat = await width(page.locator(".side-chat"));
  expect(chat).toBeGreaterThanOrEqual(355);
  expect(chat).toBeLessThan(365);
});
