// The person's own stylesheet (~/.uagent/config/style.css) overrides the
// design tokens, and an edit shows in an open page once a turn ends.
import { test, expect, selectProbeModel } from "./fixtures.js";
import { mkdir, writeFile } from "node:fs/promises";

test.beforeEach(({ command, session }) => selectProbeModel(command, session));

const accent = (page) =>
  page.evaluate(() =>
    getComputedStyle(document.documentElement)
      .getPropertyValue("--accent")
      .trim(),
  );

test("an edited stylesheet applies when a turn ends, without a reload", async ({
  page,
  session,
  host,
}) => {
  await page.goto(`/#session=${session.id}`);
  await expect(page.getByLabel("Message or guidance")).toBeVisible();
  expect(await accent(page)).not.toBe("rgb(1, 2, 3)");
  await page.evaluate(() => {
    window.kept = true;
  });
  await mkdir(`${host.home}/.uagent/config`, { recursive: true });
  await writeFile(
    `${host.home}/.uagent/config/style.css`,
    ":root[data-theme] { --accent: rgb(1, 2, 3); }\n",
  );
  await page.getByLabel("Message or guidance").fill("hello");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect.poll(() => accent(page)).toBe("rgb(1, 2, 3)");
  expect(await page.evaluate(() => window.kept)).toBe(true);
});
