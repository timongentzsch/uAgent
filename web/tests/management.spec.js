import { test, expect, withoutServiceWorker } from "./fixtures.js";
import { mkdir, writeFile } from "node:fs/promises";
import { basename, resolve } from "node:path";

// Library rows the list loaded, not the rows its placeholder draws.
const LOADED_ROWS = ".library-row:not(.placeholder *)";

{
  for (const viewport of [
    { width: 1440, height: 900 },
    { width: 390, height: 844 },
  ]) {
    test(`Library shell keeps its geometry while loading at ${viewport.width}px`, async ({
      page,
      session,
    }) => {
      await page.setViewportSize(viewport);
      const { promise: gate, resolve: release } = Promise.withResolvers();
      await page.route("**/api/library", async (route) => {
        await gate;
        await route.continue();
      });
      await page.goto(`/#session=${session.id}`);
      if (viewport.width < 900)
        await page.getByRole("button", { name: "Open conversations" }).click();
      const nav =
        viewport.width < 900
          ? page.getByRole("dialog", { name: "Conversations" })
          : page.getByRole("complementary");
      await nav.getByRole("button", { name: "Library", exact: true }).click();
      const measure = () =>
        page.evaluate(() => {
          const rect = document
            .querySelector(".management")
            .getBoundingClientRect();
          return {
            shell: {
              x: rect.x,
              y: rect.y,
              width: rect.width,
              height: rect.height,
            },
            overflow: document.documentElement.scrollWidth - innerWidth,
          };
        });
      let loading;
      try {
        await expect(
          page.getByRole("status", { name: "Loading library…" }),
        ).toBeVisible();
        loading = await measure();
      } finally {
        release();
      }
      await expect(
        page.getByRole("status", { name: "Loading library…" }),
      ).toHaveCount(0);
      const loaded = await measure();
      for (const edge of ["x", "y", "width", "height"])
        expect(
          Math.abs(loaded.shell[edge] - loading.shell[edge]),
          edge,
        ).toBeLessThan(3);
      expect(loading.overflow).toBe(0);
      expect(loaded.overflow).toBe(0);
    });
  }
}

test("the Library lists every place's things, opens them beside and moves a memory", async ({
  page,
  host: fixture,
  command,
  session,
}) => {
  await withoutServiceWorker(page);
  const design = resolve(fixture.project, "design/landing");
  await mkdir(design, { recursive: true });
  await writeFile(
    resolve(design, "hero-a.html"),
    '<!doctype html><meta name="artboard" content="390x844"><title>Hero</title><p>hi</p>',
  );
  const second = resolve(fixture.project, "../second-project");
  await mkdir(second, { recursive: true });
  await command("create", { cwd: second });
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto(`/#session=${session.id}`);
  const nav = page.getByRole("complementary");
  await nav.getByRole("button", { name: "Library", exact: true }).click();
  const group = (path) =>
    page.getByRole("region", { name: basename(path), exact: true });
  const editor = page.getByLabel("Document content");

  // A new memory is named and placed, then opens beside the list.
  // Under Everything, New asks which: a memory or a skill.
  await page
    .locator(".management-toolbar")
    .getByRole("button", { name: "New", exact: true })
    .click();
  await page.getByRole("menuitem", { name: "Memory", exact: true }).click();
  await page.getByLabel("Name", { exact: true }).fill("browser-lesson");
  await page
    .getByRole("combobox", { name: "Where", exact: true })
    .selectOption({ label: basename(fixture.project) });
  await editor.fill(
    "# Durable lesson\n\nKeep native controls shared.\n\n| Scope | Rule |\n| --- | --- |\n| All | Shared |",
  );
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(
    group(fixture.project)
      .locator(LOADED_ROWS)
      .filter({ hasText: "browser-lesson" }),
  ).toContainText("memory · Durable lesson");
  await expect(
    page.getByRole("heading", { name: "Durable lesson" }),
  ).toBeVisible();
  await expect(editor).toHaveCount(0);
  await expect(page.locator(".document-preview table")).toHaveCSS(
    "border-collapse",
    "collapse",
  );

  // An unsaved edit waits in the tab for the item's next opening.
  await page.getByRole("button", { name: "Edit", exact: true }).click();
  await editor.fill("Unsaved draft survives navigation.");
  await nav.getByRole("button", { name: "Scheduled", exact: true }).click();
  await nav.getByRole("button", { name: "Library", exact: true }).click();
  await page.locator(LOADED_ROWS).filter({ hasText: "browser-lesson" }).click();
  await expect(editor).toHaveValue("Unsaved draft survives navigation.");
  await page.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "Durable lesson" }),
  ).toBeVisible();

  // The search reads what an item says, not only its name.
  const search = page.getByLabel("Search library");
  await search.fill("native controls");
  await expect(page.locator(LOADED_ROWS)).toHaveCount(0);
  await search.fill("durable");
  await expect(page.locator(LOADED_ROWS)).toHaveCount(1);
  await search.fill("");

  // Moved to another place, it is that place's alone.
  await page.getByRole("button", { name: "Library item menu" }).click();
  await page.getByRole("menuitem", { name: "Move to…" }).click();
  await page
    .getByRole("dialog", { name: "Move browser-lesson" })
    .getByRole("combobox", { name: "Where" })
    .selectOption({ label: basename(second) });
  await page.getByRole("button", { name: "Move", exact: true }).click();
  await expect(group(second).locator(LOADED_ROWS)).toContainText(
    "browser-lesson",
  );
  await expect(
    group(fixture.project)
      .locator(LOADED_ROWS)
      .filter({ hasText: "browser-lesson" }),
  ).toHaveCount(0);

  // A design shows its pages and opens as the canvas.
  await page
    .getByRole("navigation", { name: "Library filters" })
    .getByRole("button", { name: /^Designs/ })
    .click();
  await expect(page.locator(LOADED_ROWS)).toHaveCount(1);
  await page.locator(LOADED_ROWS).filter({ hasText: "design/landing" }).click();
  await expect(page.locator(".library-pages iframe")).toHaveCount(1);
  await page.getByRole("button", { name: "Open canvas", exact: true }).click();
  // A design of one page opens as that page.
  await expect(
    page.frameLocator('iframe[title="Hero"]').getByText("hi"),
  ).toBeVisible();

  // On a phone the kinds are chips, the place a field, an item the page.
  await nav.getByRole("button", { name: "Library", exact: true }).click();
  await page.setViewportSize({ width: 390, height: 844 });
  const place = page.getByLabel("Place", { exact: true });
  await expect(place).toBeVisible();
  await place.selectOption({ label: basename(second) });
  await page.getByRole("button", { name: /^Everything/ }).click();
  await page.locator(LOADED_ROWS).filter({ hasText: "browser-lesson" }).click();
  await expect(
    page.getByRole("heading", { name: "Durable lesson" }),
  ).toBeVisible();
  await expect(page.locator(LOADED_ROWS)).not.toBeVisible();
  await page.getByRole("button", { name: "Back", exact: true }).click();
  await expect(page.locator(LOADED_ROWS)).toBeVisible();
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
});

test("scheduled tasks run, report and pause", async ({
  page,
  host: fixture,
}) => {
  await withoutServiceWorker(page);
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto("/");
  await expect(
    page.getByRole("status").filter({ hasText: /^Connected$/ }),
  ).toBeVisible();
  const nav = page.getByRole("complementary");
  await nav.getByRole("button", { name: "Scheduled", exact: true }).click();
  await page.getByRole("button", { name: "New task", exact: true }).click();
  await page
    .getByLabel("Name", { exact: true })
    .fill("Browser scheduled review");
  await page.getByLabel("Folder", { exact: true }).fill(fixture.project);
  await page.getByLabel("Folder", { exact: true }).press("Tab");
  await page
    .getByLabel("Instructions", { exact: true })
    .fill("Review the workspace");
  await page.getByLabel("Environment", { exact: true }).selectOption("local");
  await page.getByLabel("Approval", { exact: true }).selectOption("yolo");
  await page.getByRole("button", { name: "Task model", exact: true }).click();
  await page.getByLabel("Model", { exact: true }).fill("model-b");
  await page.getByRole("option").filter({ hasText: "mock/model-b" }).click();
  await page.getByLabel("Effort", { exact: true }).selectOption("high");
  await page.getByRole("button", { name: "Apply", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "Task model", exact: true }),
  ).toContainText("mock/model-b:high");
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "Run now", exact: true }),
  ).toBeEnabled();
  await page.getByRole("button", { name: "Run now", exact: true }).click();
  const run = page.locator(".run-row").first();
  await expect(run).toContainText("Completed", { timeout: 20000 });
  await expect(nav.getByLabel("Unread scheduled results")).toBeVisible();
  await run.getByRole("button").first().click();
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  await nav.getByRole("button", { name: "Scheduled", exact: true }).click();
  // A check that finds nothing records the run without asking a model.
  await page.getByLabel("Check · optional", { exact: true }).fill("exit 1");
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "Run now", exact: true }),
  ).toBeEnabled();
  await page.getByRole("button", { name: "Run now", exact: true }).click();
  await expect(page.locator(".run-row").first()).toContainText("Nothing new", {
    timeout: 20000,
  });
  await page.getByRole("button", { name: "Task menu", exact: true }).click();
  await page.getByRole("menuitem", { name: "Pause", exact: true }).click();
  await expect(
    page.locator(LOADED_ROWS).filter({ hasText: "Browser scheduled review" }),
  ).toContainText("Paused");
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(
    page.getByText("Calculating upcoming runs…", { exact: true }),
  ).not.toBeVisible();
  await expect
    .poll(() =>
      page.evaluate(
        () => document.querySelector(".shell").getBoundingClientRect().height,
      ),
    )
    .toBe(844);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
});

test("a skill kept by Claude Code is imported from the Library", async ({
  page,
  host: fixture,
  session,
}) => {
  await withoutServiceWorker(page);
  const skill = resolve(fixture.home, ".claude/skills/synced/id/demo");
  await mkdir(skill, { recursive: true });
  await writeFile(
    resolve(skill, "SKILL.md"),
    "---\ndescription: Demo skill\n---\nRun it with `Bash`.\n",
  );
  await page.goto(`/#session=${session.id}`);
  await page
    .getByRole("complementary")
    .getByRole("button", { name: "Library", exact: true })
    .click();
  await page.getByRole("button", { name: /^Skills/ }).click();
  await page.getByRole("button", { name: "Import", exact: true }).click();
  const dialog = page.getByRole("dialog", {
    name: "Import from Claude Code and Codex",
  });
  await expect(dialog).toContainText("demo");
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toContainText("Already in µAgent");
  await dialog.getByRole("button", { name: "Close" }).click();
  await expect(
    page
      .getByRole("region", { name: "Everywhere", exact: true })
      .locator(LOADED_ROWS)
      .filter({ hasText: /demo.*skill · Demo skill/ }),
  ).toBeVisible();
});
