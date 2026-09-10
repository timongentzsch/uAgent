import { test, expect } from "./fixtures.js";
import { mkdir } from "node:fs/promises";
import { resolve } from "node:path";

test("library drafts, shared controls and scheduled results", async ({
  page,
  host: fixture,
}, testInfo) => {
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  await page.route("**/sw.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "" }),
  );
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();

  const nav = page.getByRole("complementary");
  await nav.getByRole("button", { name: "Library", exact: true }).click();
  await page.getByLabel("Project", { exact: true }).fill(fixture.project);
  await page.getByLabel("Project", { exact: true }).press("Tab");
  await page.getByRole("button", { name: "Add memory", exact: true }).click();
  await page.getByLabel("Name", { exact: true }).fill("browser-lesson");
  const editor = page.getByLabel("Document content");
  await editor.fill("# Durable lesson\n\nKeep native controls shared.");
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(
    page.locator(".library-row").filter({ hasText: "browser-lesson" }),
  ).toBeVisible();
  await expect(
    page.getByRole("heading", { name: "Durable lesson" }),
  ).toBeVisible();
  await expect(editor).toHaveCount(0);
  await expect(
    page.getByRole("button", { name: /^(Source|Preview)$/ }),
  ).toHaveCount(0);
  await page.getByRole("button", { name: "Edit", exact: true }).click();
  await editor.fill("Unsaved draft survives navigation.");
  await nav.getByRole("button", { name: "Scheduled", exact: true }).click();
  await nav.getByRole("button", { name: "Library", exact: true }).click();
  await page.getByLabel("Project", { exact: true }).fill(fixture.project);
  await page.getByLabel("Project", { exact: true }).press("Tab");
  await expect(editor).toHaveValue("Unsaved draft survives navigation.");
  await page.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(editor).toHaveCount(0);
  await expect(
    page.getByRole("heading", { name: "Durable lesson" }),
  ).toBeVisible();
  await page.screenshot({
    path: testInfo.outputPath("library-desktop.png"),
    fullPage: true,
  });
  const projectGroup = page.getByRole("region", {
    name: fixture.project,
    exact: true,
  });
  await expect(projectGroup.locator(".library-row")).toContainText(
    "browser-lesson",
  );
  await page.getByRole("button", { name: "Edit", exact: true }).click();
  await editor.fill("Draft belongs to the first project.");
  const secondProject = resolve(fixture.project, "../second-project");
  await mkdir(secondProject, { recursive: true });
  let releaseList;
  const listReady = new Promise((resolve) => {
    releaseList = resolve;
  });
  await page.route("**/api/command", async (route) => {
    const request = route.request().postDataJSON();
    if (
      request.kind === "memory" &&
      request.action === "list" &&
      request.cwd === secondProject
    )
      await listReady;
    await route.continue();
  });
  await page.getByLabel("Project", { exact: true }).fill(secondProject);
  await page.getByLabel("Project", { exact: true }).press("Tab");
  const secondGroup = page.getByRole("region", {
    name: secondProject,
    exact: true,
  });
  try {
    await expect(secondGroup.getByRole("status")).toBeVisible();
    await expect(secondGroup.locator(".library-row")).toHaveCount(0);
  } finally {
    releaseList();
  }
  await page.unroute("**/api/command");
  await expect(
    secondGroup.getByText("No memories found.", { exact: true }),
  ).toBeVisible();
  await expect(projectGroup.locator(".library-row")).toHaveCount(0);
  await page.getByRole("button", { name: "Add memory", exact: true }).click();
  await page.getByLabel("Name", { exact: true }).fill("browser-lesson");
  await editor.fill("This belongs only to the second project.");
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(secondGroup.locator(".library-row")).toContainText(
    "browser-lesson",
  );
  const filter = page.getByLabel("Filter library");
  await filter.selectOption(`project:${fixture.project}`);
  await expect(page.getByLabel("Project", { exact: true })).toHaveValue(
    fixture.project,
  );
  await expect(secondGroup).toHaveCount(0);
  await expect(
    page.getByRole("region", { name: "Global memories", exact: true }),
  ).toHaveCount(0);
  await expect(editor).toHaveValue("Draft belongs to the first project.");
  await page.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "Durable lesson" }),
  ).toBeVisible();
  await filter.selectOption("global");
  const globalGroup = page.getByRole("region", {
    name: "Global memories",
    exact: true,
  });
  await expect(globalGroup).toBeVisible();
  await expect(projectGroup).toHaveCount(0);
  await filter.selectOption("all");
  await secondGroup.getByRole("button", { expanded: false }).click();
  await secondGroup.locator(".library-row").click();
  await expect(page.locator(".document-preview")).toHaveText(
    "This belongs only to the second project.",
  );
  await projectGroup.getByRole("button", { expanded: false }).click();
  await projectGroup.locator(".library-row").click();
  await expect(
    page.getByRole("heading", { name: "Durable lesson" }),
  ).toBeVisible();
  await page.screenshot({
    path: testInfo.outputPath("library-folder-groups.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.getByRole("button", { name: "Back", exact: true }).click();
  await filter.selectOption(`project:${secondProject}`);
  await expect(secondGroup.locator(".library-row")).toBeVisible();
  await expect(secondGroup.getByRole("status")).toHaveCount(0);
  await expect(projectGroup).toHaveCount(0);
  await page.screenshot({
    path: testInfo.outputPath("library-mobile-folders.png"),
    fullPage: true,
  });
  await secondGroup.locator(".library-row").click();
  await expect(page.locator(".document-preview")).toHaveText(
    "This belongs only to the second project.",
  );
  await page.getByRole("button", { name: "Edit", exact: true }).click();
  await editor.fill("Temporary mobile edit.");
  await page.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(editor).toHaveCount(0);
  await expect(page.locator(".document-preview")).toHaveText(
    "This belongs only to the second project.",
  );
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  await page.setViewportSize({ width: 1440, height: 1000 });
  await filter.selectOption(`project:${fixture.project}`);
  await page.getByRole("button", { name: "Skills", exact: true }).click();
  await expect(filter).toHaveValue("project");
  await page.getByRole("button", { name: "Add skill", exact: true }).click();
  await page.getByLabel("Name", { exact: true }).fill("browser-review");
  await editor.fill("---\ndescription: Review a change\n---\nCheck the diff.");
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(
    page.locator(".library-row").filter({ hasText: "browser-review" }),
  ).toBeVisible();
  await nav.getByRole("button", { name: "Scheduled", exact: true }).click();
  await page.getByRole("button", { name: "New task", exact: true }).click();
  await page
    .getByLabel("Name", { exact: true })
    .fill("Browser scheduled review");
  await page.getByLabel("Project", { exact: true }).fill(fixture.project);
  await page.getByLabel("Project", { exact: true }).press("Tab");
  await page
    .getByLabel("Instructions", { exact: true })
    .fill("Review the workspace");
  await page.getByLabel("Environment", { exact: true }).selectOption("local");
  await page.getByLabel("Permissions", { exact: true }).selectOption("yolo");
  await page.getByRole("button", { name: "Task model", exact: true }).click();
  await page.getByLabel("Model", { exact: true }).selectOption("mock/model-b");
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
  await expect(run).toContainText("completed", { timeout: 20000 });
  await expect(nav.getByLabel("Unread scheduled results")).toBeVisible();
  await page.screenshot({
    path: testInfo.outputPath("scheduled-desktop.png"),
    fullPage: true,
  });
  await run.getByRole("button").first().click();
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible();
  await nav.getByRole("button", { name: "Scheduled", exact: true }).click();
  await page.getByRole("button", { name: "Task menu", exact: true }).click();
  await page.getByRole("menuitem", { name: "Pause", exact: true }).click();
  await expect(
    page
      .locator(".library-row")
      .filter({ hasText: "Browser scheduled review" }),
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
  await page.screenshot({
    path: testInfo.outputPath("scheduled-mobile.png"),
    fullPage: true,
  });
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  expect(errors).toEqual([]);
});
