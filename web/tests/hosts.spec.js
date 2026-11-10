import { test, expect, savedConversation } from "./fixtures.js";
// Mocked host APIs must stay intercepted across reload and worker updates.
test.use({ serviceWorkers: "block" });
const remote = "b".repeat(32);
async function offlineHost(page, phase = "disconnected") {
  // These API mocks exercise a client without the separate PWA lifecycle.
  await page.addInitScript(() => {
    delete Navigator.prototype.serviceWorker;
  });
  let reads = 0;
  const actions = [];
  await page.route("**/api/hosts", async (route) => {
    if (route.request().method() === "POST") {
      actions.push(route.request().postDataJSON());
      await route.fulfill({ json: { id: remote } });
    } else {
      reads++;
      await route.fulfill({
        json: {
          hosts: [
            {
              id: remote,
              name: "GPU",
              address: "GPU",
              phase,
              error: "",
            },
          ],
        },
      });
    }
  });
  await page.route(/\/api\/sessions(?:\?.*)?$/, async (route) => {
    const response = await route.fetch();
    const catalogue = await response.json();
    catalogue.sessions.push({
      id: "a".repeat(32),
      host: remote,
      title: "GPU conversation",
      cwd: "/srv/project",
      updated: Date.now(),
      turns: 1,
      status: "saved",
      generation: "",
    });
    await route.fulfill({ json: catalogue });
  });
  return { actions, reads: () => reads };
}
test("restored hosts stay disconnected; Recent explicitly opens the owning host", async ({
  page,
  host,
  request,
}) => {
  const local = await savedConversation(host, request, "Local conversation");
  const observed = await offlineHost(page);
  await page.goto(`/#host=${remote}`);
  await expect(
    page
      .locator(".host-list")
      .getByRole("button", { name: /GPU.*Disconnected/ }),
  ).toBeVisible();
  await expect.poll(observed.reads).toBeGreaterThanOrEqual(2);
  expect(observed.actions).toEqual([]);
  const recent = page.getByRole("group", { name: "Recent", exact: true });
  await expect(
    recent.getByRole("button", { name: "Recent", exact: true }),
  ).toHaveAttribute("aria-expanded", "false");
  await recent.getByRole("button", { name: "Recent", exact: true }).click();
  await expect(
    recent.getByRole("button", { name: "GPU conversation", exact: true }),
  ).toBeVisible();
  expect(observed.actions).toEqual([]);
  await recent
    .getByRole("button", { name: "Local conversation", exact: true })
    .click();
  await expect(page).toHaveURL(new RegExp(`#session=${local.id}$`));
  await expect(
    page.getByRole("heading", { name: "Local conversation", level: 1 }),
  ).toBeVisible();
  await recent
    .getByRole("button", { name: "GPU conversation", exact: true })
    .click();
  await expect.poll(() => observed.actions.length).toBe(1);
  expect(observed.actions[0]).toEqual({ action: "connect", id: remote });
  await expect(page).toHaveURL(
    new RegExp(`host=${remote}.*session=${"a".repeat(32)}`),
  );
  await page.reload();
  await expect(
    page
      .locator(".host-list")
      .getByRole("button", { name: /GPU.*Disconnected/ }),
  ).toBeVisible();
  expect(observed.actions).toHaveLength(1);
});
test.describe("phone host controls", () => {
  test.use({
    isMobile: true,
    hasTouch: true,
    viewport: { width: 390, height: 844 },
  });
  test("host details expose connection controls without a host switch", async ({
    page,
  }) => {
    const observed = await offlineHost(page);
    await page.goto("/");
    await page
      .getByRole("button", { name: "Open conversations", exact: true })
      .tap();
    const row = page
      .locator(".host-list")
      .getByRole("button", { name: /GPU.*Disconnected/ });
    await expect(row).toBeVisible();
    expect((await row.boundingBox()).height).toBeGreaterThanOrEqual(44);
    await expect(row.locator(".host-square")).toHaveCSS("border-radius", "0px");
    await row.tap();
    const detail = page.getByRole("dialog", { name: "GPU", exact: true });
    await expect(
      detail.getByRole("button", { name: "Reconnect", exact: true }),
    ).toBeEnabled();
    await expect(
      detail.getByRole("button", { name: "Disconnect", exact: true }),
    ).toBeDisabled();
    await expect(detail.getByLabel("Folder path")).toHaveCount(0);
    await expect(
      detail.getByRole("button", { name: /Switch.*host|Open host/ }),
    ).toHaveCount(0);
    await detail.getByRole("button", { name: "Reconnect", exact: true }).tap();
    await expect.poll(() => observed.actions.length).toBe(1);
    expect(observed.actions[0]).toEqual({ action: "reconnect", id: remote });
  });
});

for (const width of [390, 1440]) {
  test.describe(`${width}px creation`, () => {
    test.use({
      viewport: { width, height: 844 },
      isMobile: width < 600,
      hasTouch: width < 600,
    });
    test(`home chooses a host and folder at ${width}px`, async ({ page }) => {
      await offlineHost(page, "connected");
      const creations = [];
      await page.route("**/api/folders?*", async (route) => {
        const query = new URL(route.request().url()).searchParams;
        const remote = query.has("host");
        const root = remote ? "/srv" : "/home/local";
        const path = query.get("path") === "~" ? root : query.get("path");
        await route.fulfill({
          json: {
            path,
            parent: "/",
            more: false,
            entries:
              path === root
                ? [{ name: "projects", dir: true }]
                : [{ name: "sample", dir: true }],
          },
        });
      });
      await page.route("**/api/command", async (route) => {
        const command = route.request().postDataJSON();
        if (command.kind !== "create") return route.continue();
        creations.push(command);
        await route.fulfill({
          json: {
            request_id: command.request_id,
            accepted: false,
            error: "Fixture stops after folder selection",
          },
        });
      });
      await page.goto("/");
      await page
        .getByRole("button", { name: "New conversation", exact: true })
        .click();
      const dialog = page.getByRole("dialog", {
        name: "New conversation",
        exact: true,
      });
      await dialog
        .getByRole("combobox", { name: "Host", exact: true })
        .selectOption(remote);
      await expect(dialog.getByLabel("Folder on the host")).toHaveValue("/srv");
      await dialog
        .getByRole("button", { name: "projects", exact: true })
        .click();
      await dialog.getByRole("button", { name: "sample", exact: true }).click();
      await expect(dialog.getByLabel("Folder on the host")).toHaveValue(
        "/srv/projects/sample",
      );
      expect(
        await page.evaluate(() => document.documentElement.scrollWidth),
      ).toBeLessThanOrEqual(width);
      await page.screenshot({
        path: test.info().outputPath("host-folder-picker.png"),
      });
      await dialog
        .getByRole("button", { name: "Start conversation", exact: true })
        .click();
      await expect.poll(() => creations.length).toBe(1);
      expect(creations[0]).toMatchObject({
        host: remote,
        cwd: "/srv/projects/sample",
      });
    });
  });
}

test("host details show measured main-to-host latency without a switch action", async ({
  page,
}) => {
  await offlineHost(page, "connected");
  await page.route("**/api/hosts/*/ping", (route) =>
    route.fulfill({ json: { round_trip_ms: 27.4 } }),
  );
  await page.goto("/");
  await page
    .locator(".host-list")
    .getByRole("button", { name: /GPU.*Connected/ })
    .click();
  const dialog = page.getByRole("dialog", { name: "GPU", exact: true });
  await expect(dialog.getByRole("status")).toHaveText(
    "Main → host · 27 ms round trip",
  );
  await expect(
    dialog.getByRole("button", { name: /Switch.*host|Open host/ }),
  ).toHaveCount(0);
  await expect(dialog.getByLabel("Folder path")).toHaveCount(0);
});
