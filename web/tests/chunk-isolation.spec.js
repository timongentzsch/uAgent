// Transcript must not statically import lazy dialog chunks: holding every
// dialog chunk must not block message or turn-footer rendering.
import { test, expect } from "./fixtures.js";
import { readFile } from "node:fs/promises";

test("transcript renders while dialog chunks are held", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  const manifest = JSON.parse(
    await readFile(new URL("../dist/.vite/manifest.json", import.meta.url)),
  );
  // Every lazily loaded feature chunk (dialogs and pages), from the build
  // manifest, so a new one is held too. Keyed by module name.
  const chunks = Object.entries(manifest).filter(
    ([source, chunk]) =>
      chunk.isDynamicEntry && source.startsWith("src/features/"),
  );
  expect(chunks.length).toBeGreaterThan(5);
  const gates = {};
  for (const [source, chunk] of chunks) {
    let release;
    const gate = new Promise((resolve) => (release = resolve));
    gates[
      source
        .split("/")
        .pop()
        .replace(/\.tsx?$/, "")
    ] = release;
    await page.route(`**/${chunk.file}`, async (route) => {
      await gate;
      await route.continue();
    });
  }
  const release = async (key) => {
    gates[key]();
    await page.waitForTimeout(200);
  };

  // The deliberately held lazy resources keep document readiness open. The
  // response commit is sufficient to release the shell chunks without making
  // navigation wait on the resources this test is intentionally gating.
  await page.goto(`/#session=${session.id}`, { waitUntil: "commit" });
  const prompt = page.getByLabel("Message or guidance");
  await expect(prompt).toBeVisible();

  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
  await command("permissions", {
    session_id: session.id,
    generation: session.generation,
    mode: "yolo",
  });
  await prompt.fill("Exploration probe");
  await prompt.press("Enter");
  // Dialog chunks (statistics included) are still held: the tool rows
  // and the turn footer button must render anyway.
  // Both read-only calls fold into one Explored row.
  await expect(page.locator(".transcript .tool-disclosure")).toHaveCount(1, {
    timeout: 60000,
  });
  await expect(page.locator(".transcript .group")).toContainText("Explored");
  await expect(
    page.getByRole("button", { name: /^Turn statistics: / }).first(),
  ).toBeVisible({ timeout: 30000 });
  // Its styles ship with the transcript (message.css), not the lazy
  // statistics chunk: already laid out before the dialog ever opens.
  await expect(
    page.getByRole("button", { name: /^Turn statistics: / }).first(),
  ).toHaveCSS("display", "flex");

  // Releasing the statistics chunk must light up the dialog on demand.
  await release("statistics");
  await page
    .getByRole("button", { name: /^Turn statistics: / })
    .first()
    .click();
  await expect(page.locator("dialog dl.stats").first()).toBeVisible({
    timeout: 15000,
  });
});
