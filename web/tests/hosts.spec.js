import { test, expect, savedConversation } from "./fixtures.js";
// Mocked host APIs must stay intercepted across reload and worker updates.
test.use({ serviceWorkers: "block" });
const remote = "b".repeat(32);
async function offlineHost(page) {
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
              phase: "disconnected",
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
  test("host details expose reconnect and folder actions", async ({ page }) => {
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
    await expect(detail.getByLabel("Folder path")).toBeVisible();
    await detail.getByRole("button", { name: "Reconnect", exact: true }).tap();
    await expect.poll(() => observed.actions.length).toBe(1);
    expect(observed.actions[0]).toEqual({ action: "reconnect", id: remote });
  });
});
