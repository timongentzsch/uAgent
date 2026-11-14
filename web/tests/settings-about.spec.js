// The About page: this build, the newest release as the host reads it from
// GitHub, and how to update. The test host is a build, not an install, so
// it offers the command, not the button.
import { test, expect } from "./fixtures.js";

test("About checks for a release, shows its notes and how to update", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await expect(page.locator(".composer .activity-button")).toHaveCount(0);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await page
    .locator(".settings-nav")
    .getByRole("button", { name: "About", exact: true })
    .click();
  const pane = page.locator(".settings-pane");
  await expect(pane.locator(".row", { hasText: "Version" })).toContainText(
    /\d+\.\d+\.\d+/,
  );
  await expect(pane.locator(".row", { hasText: "Installed as" })).toContainText(
    "Not recorded",
  );
  await expect(pane).toContainText("Not checked yet");

  await pane.getByRole("button", { name: "Check for updates" }).click();
  await expect(pane).toContainText("µAgent 999.0.0 is available");
  await expect(pane.locator("time")).toBeVisible();
  const command = pane.locator(".row", { hasText: "install.sh | sh" });
  await expect(command).toContainText("Run this on the host.");
  await expect(
    command.getByRole("button", { name: "Copy code" }),
  ).toBeVisible();
  await expect(pane.getByRole("button", { name: /^Update to/ })).toHaveCount(0);

  await pane.getByRole("button", { name: "What is new" }).click();
  const notes = page.getByRole("dialog", { name: "µAgent 999.0.0" });
  await expect(notes.getByRole("heading", { name: "Faster" })).toBeVisible();
  await expect(notes).toContainText("Sheets open sooner");
  await expect(
    notes.getByRole("button", { name: "Open on GitHub" }),
  ).toBeVisible();
});

test("About says which Chrome a container holds, and looks again when asked", async ({
  page,
  session,
}) => {
  const asked = [];
  const chrome = { status: "ready 157.0.1-1", checked_ms: Date.now() - 3600e3 };
  await page.route("**/api/command", async (route) => {
    const body = route.request().postDataJSON();
    if (body.kind !== "update") return route.continue();
    asked.push(body.action || "show");
    if (body.action === "chrome") chrome.status = "ready 158.0.2-1";
    const response = await route.fetch({
      postData: JSON.stringify({ ...body, action: "show" }),
    });
    const outcome = await response.json();
    outcome.result.chrome = { ...chrome, checking: false };
    await route.fulfill({ response, json: outcome });
  });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await page
    .locator(".settings-nav")
    .getByRole("button", { name: "About", exact: true })
    .click();
  const group = page.getByRole("region", { name: "Chrome", exact: true });
  await expect(group).toContainText("Chrome 157.0.1-1");
  await expect(group.locator("time")).toBeVisible();
  await group.getByRole("button", { name: "Check now" }).click();
  await expect(group).toContainText("Chrome 158.0.2-1");
  expect(asked).toContain("chrome");
});
