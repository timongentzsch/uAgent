// The named providers are edited as what they are: a name, an address, a
// key and the API it speaks. A key is never shown, and one that is not
// retyped is kept. The whole file can be read beside the form.
import { test, expect } from "./fixtures.js";
import { readFile } from "node:fs/promises";

test("a provider is added, edited and kept, and the file is shown without its key", async ({
  page,
  session,
  host,
}) => {
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

  await form.getByLabel("New provider name").fill("lab");
  await form.getByLabel("New provider address").fill("https://lab.example/v1");
  await form.getByRole("button", { name: "Add", exact: true }).click();
  const lab = form.getByRole("region", { name: "lab", exact: true });
  await expect(lab.getByLabel("lab address")).toHaveValue(
    "https://lab.example/v1",
  );

  // The key is taken and never shown again.
  const key = lab.getByLabel("lab key");
  await key.fill("sk-lab-secret");
  await key.press("Enter");
  await expect(key).toHaveAttribute("placeholder", "Set · enter a replacement");
  await expect(key).toHaveValue("");

  // Another field changes; the key that was not retyped stays.
  await lab.getByLabel("lab API").selectOption("responses");
  const address = lab.getByLabel("lab address");
  await address.fill("https://lab.example/v2");
  await address.press("Enter");
  await expect
    .poll(async () => {
      const saved = JSON.parse(
        await readFile(`${host.home}/.uagent/config/settings.json`, "utf8"),
      );
      return saved.all.providers?.lab;
    })
    .toEqual({
      base_url: "https://lab.example/v2",
      api_key: "sk-lab-secret",
      wire_api: "responses",
    });

  // The file as it stands, with the key hidden.
  await page.getByRole("button", { name: "Show the file" }).click();
  const file = form.locator(".configuration-file");
  await expect(file).toContainText('"base_url": "https://lab.example/v2"');
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
  await lab.getByRole("button", { name: "Remove" }).click();
  await expect(lab).toHaveCount(0);
});
