// The named providers are a list of their own, one row each, and a provider
// is edited in a sheet: a name, an address, a key, the API it speaks and the
// tools it runs itself. A key is never shown, and one that is not retyped is
// kept, under a new name too. The whole file can be read beside the form.
import { test, expect } from "./fixtures.js";
import { readFile } from "node:fs/promises";

test("a provider is added, edited, renamed and removed in its sheet", async ({
  page,
  session,
  host,
}) => {
  const providers = async () =>
    JSON.parse(
      await readFile(`${host.home}/.uagent/config/settings.json`, "utf8"),
    )?.all?.providers;
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(`/#session=${session.id}`);
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await page
    .locator(".settings-nav")
    .getByRole("button", { name: "All conversations", exact: true })
    .click();
  const form = page.locator(".configuration");
  await page.getByLabel("Find a setting").fill("Named providers");

  await form.getByRole("button", { name: "Add a provider" }).click();
  const adding = page.getByRole("dialog", { name: "Add a provider" });
  await adding.getByLabel(/^Name/).fill("lab");
  await adding.getByLabel(/^Address/).fill("https://lab.example/v1");
  await adding.getByLabel(/^Key/).fill("sk-lab-secret");
  await adding.getByRole("button", { name: "Save" }).click();
  await expect(adding).toHaveCount(0);
  const row = form.locator(".row", { hasText: "https://lab.example/v1" });
  await expect(row).toContainText("Chat completions");

  // The key is taken and never shown again; it is kept as other fields
  // change, and a tool the endpoint runs is defined as its API takes it.
  await form.getByRole("button", { name: "Edit lab", exact: true }).click();
  const sheet = page.getByRole("dialog", { name: "Edit provider" });
  const key = sheet.getByLabel(/^Key/);
  await expect(key).toHaveAttribute("placeholder", "••••••••");
  await expect(key).toHaveValue("");
  await sheet.getByLabel(/^Speaks/).selectOption("responses");
  await sheet.getByRole("button", { name: "Define a hosted tool" }).click();
  const definition = sheet.getByLabel("Definition");
  await definition.fill("[]");
  await sheet.getByRole("button", { name: "Done" }).click();
  await expect(sheet.getByRole("alert")).toContainText('"type"');
  const tool = { type: "code_execution_20250825", name: "code_execution" };
  await definition.fill(JSON.stringify(tool));
  await sheet.getByRole("button", { name: "Done" }).click();
  await expect(
    sheet.getByRole("button", { name: "Edit code_execution" }),
  ).toBeVisible();
  await sheet.getByLabel("Web search").check();
  await sheet.getByRole("button", { name: "Save" }).click();
  await expect(sheet).toHaveCount(0);
  await expect(row).toContainText("Responses");
  await expect
    .poll(async () => (await providers())?.lab)
    .toEqual({
      base_url: "https://lab.example/v1",
      api_key: "sk-lab-secret",
      wire_api: "responses",
      hosted_tools: ["web_search", tool],
    });

  // The file as it stands, with the key hidden.
  await page.getByRole("button", { name: "Show the file" }).click();
  const file = form.locator(".configuration-file");
  await expect(file).toContainText('"base_url": "https://lab.example/v1"');
  await expect(file).toContainText('"api_key": "<redacted>"');
  await expect(file).not.toContainText("sk-lab-secret");
  if (process.env.UAGENT_SCREENSHOTS) {
    await page.screenshot({
      path: `${process.env.UAGENT_SCREENSHOTS}/settings-file.png`,
    });
  }
  await page.getByRole("button", { name: "Show the form" }).click();
  if (process.env.UAGENT_SCREENSHOTS) {
    await page.screenshot({
      path: `${process.env.UAGENT_SCREENSHOTS}/settings-providers.png`,
    });
  }

  // Renamed, it keeps its hidden key.
  await form.getByRole("button", { name: "Edit lab", exact: true }).click();
  await sheet.getByLabel(/^Name/).fill("lab2");
  await sheet.getByRole("button", { name: "Save" }).click();
  await expect(sheet).toHaveCount(0);
  await expect
    .poll(async () => {
      const saved = await providers();
      return ["lab" in saved, saved.lab2.api_key, saved.lab2.renamed_from];
    })
    .toEqual([false, "sk-lab-secret", undefined]);

  await form.getByRole("button", { name: "Edit lab2", exact: true }).click();
  await sheet.getByRole("button", { name: "Remove" }).click();
  await expect(sheet).toHaveCount(0);
  await expect(row).toHaveCount(0);
  await expect.poll(async () => (await providers())?.lab2).toBeUndefined();
});
