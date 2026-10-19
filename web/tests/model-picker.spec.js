import { test, expect, withoutServiceWorker } from "./fixtures.js";

for (const width of [390, 1280]) {
  test(`model search and catalogue skeleton at ${width}px`, async ({
    page,
    session,
  }, testInfo) => {
    await page.setViewportSize({ width, height: 900 });
    await withoutServiceWorker(page);
    let release;
    const gate = new Promise((resolve) => (release = resolve));
    await page.route("**/api/command", async (route) => {
      const body = route.request().postDataJSON();
      if (body.kind !== "model" || body.operation !== "catalog")
        return route.continue();
      await gate;
      await route.fulfill({
        json: {
          v: 2,
          accepted: true,
          pending: false,
          request_id: body.request_id,
          result: {
            models: [
              {
                value: "mock/model-a",
                label: "mock/model-a",
                name: "Alpha",
                active: true,
              },
              {
                value: "mock/model-b",
                label: "mock/model-b",
                name: "Beta · Detailed model display name for a provider with a long description",
                efforts: ["high", "low"],
              },
            ],
          },
        },
      });
    });
    await page.goto(`/#session=${session.id}`);
    await page
      .getByRole("button", { name: "Model and effort", exact: true })
      .click();
    const picker = page.getByRole("dialog", {
      name: "Model and effort",
      exact: true,
    });
    await expect(
      picker.getByRole("status", { name: "Loading models…" }),
    ).toBeVisible();
    await expect(
      picker.getByRole("button", { name: "Apply", exact: true }),
    ).toBeDisabled();
    await expect(picker.locator(".placeholder .text-control")).toHaveCount(2);
    await expect(picker.locator(".placeholder [role=switch]")).toHaveCount(1);
    await expect(picker.getByText("Variant", { exact: true })).toHaveCount(0);
    await picker.screenshot({
      path: testInfo.outputPath(`model-loading-${width}.png`),
    });
    const loading = await picker.boundingBox();
    release();
    const input = picker.getByRole("combobox", { name: "Model", exact: true });
    await expect(input).toBeEnabled();
    await expect(input).toBeFocused();
    await expect(input).toHaveValue("mock/model-a");
    const loaded = await picker.boundingBox();
    expect(Math.abs(loaded.height - loading.height)).toBeLessThan(3);
    await input.fill("beta high");
    await expect(picker.getByRole("listbox").getByRole("option")).toHaveCount(
      1,
    );
    await expect(picker.getByRole("listbox").getByRole("option")).toContainText(
      "mock/model-b",
    );
    await testInfo.attach(`model-search-${width}`, {
      body: await picker.screenshot({
        path: testInfo.outputPath(`model-search-${width}.png`),
      }),
      contentType: "image/png",
    });
    await input.fill("no such model");
    await expect(picker.getByRole("status")).toHaveText("No matching options.");
    await input.press("Enter");
    await expect(picker).toBeVisible();
    await input.press("Escape");
    await expect(input).toHaveValue("mock/model-a");
    await expect(input).toHaveAttribute("aria-expanded", "false");
    await expect(picker).toBeVisible();
    await input.fill("beta");
    await input.press("ArrowDown");
    await input.press("Enter");
    await expect(input).toHaveValue("mock/model-b");
    await expect(input).toHaveAttribute("aria-expanded", "false");
    await picker.getByLabel("Effort", { exact: true }).selectOption("high");
    await picker.getByRole("button", { name: "Apply", exact: true }).click();
    await expect(picker).toHaveCount(0);
    await expect(
      page.getByRole("button", { name: "Model and effort", exact: true }),
    ).toHaveText("mock/model-b:high");
  });
}
