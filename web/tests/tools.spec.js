import { test, expect } from "./fixtures.js";

test("tool catalogue reports exact schema bytes and persists selection", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1200, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Tools", exact: true }).click();

  const dialog = page.getByRole("dialog", { name: "Tools", exact: true });
  await expect(dialog.getByText(/serialized schema bytes/)).toBeVisible();
  await expect(dialog.getByText(/estimated.*tokens/i)).toHaveCount(0);

  const profile = dialog.getByRole("combobox", { name: "Tool profile" });
  await profile.selectOption("minimal");
  await expect(profile).toHaveValue("minimal");

  const search = dialog.getByRole("searchbox", { name: "Find a tool" });
  await search.fill("read_path");
  const choice = dialog.locator(".tool-choice");
  await expect(choice).toHaveCount(1);
  await expect(choice.locator("code")).toHaveText("read_path");
  const enabled = choice.getByRole("checkbox");
  await expect(enabled).toBeChecked();
  await enabled.uncheck();
  await expect(enabled).not.toBeChecked();
});
