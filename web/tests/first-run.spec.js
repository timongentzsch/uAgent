// @webkit
// What a person meets first: pairing from a printed link, and a first
// message the host refuses.
import {
  connected,
  DEV_SERVER_URL,
  devServerTest,
  expect,
  fakeStream,
  mockHost,
  test,
} from "./fixtures.js";

test.describe("not paired yet", () => {
  test.use({ paired: false });

  test("a pairing link pasted into the Connect page's tab pairs it", async ({
    page,
    host,
  }) => {
    await page.goto("/");
    await expect(
      page.getByRole("heading", { name: "Connect device" }),
    ).toBeVisible();
    // Only the hash changes: the page is not loaded again.
    await page.evaluate((code) => {
      location.hash = `#pair=${code}`;
    }, host.code);
    await expect(connected(page)).toBeVisible();
    expect(new URL(page.url()).hash).not.toContain("pair=");
  });
});

for (const width of [1280, 390])
  test(`a first message refused in the command's reply stays in the draft, its notice clear of the title and leading to Providers at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    await page.route("**/api/command", (route) => {
      const body = route.request().postDataJSON();
      return body?.kind === "activate"
        ? route.fulfill({
            json: {
              request_id: body.request_id,
              accepted: false,
              error: "no provider: add a provider",
              // The host names the refusal by its code; the client keys on it.
              code: "no_provider",
            },
          })
        : route.continue();
    });
    await page.goto("/");
    await page.getByLabel("Start a conversation").fill("hello");
    await page.getByRole("button", { name: "Start", exact: true }).click();
    const notice = page.locator(".toasts .banner");
    await expect(notice).toContainText("no provider");
    await expect(page.getByLabel("Message or guidance")).toHaveValue("hello");
    const title = await page.locator(".conversation-head h1").boundingBox();
    const toast = await notice.boundingBox();
    expect(toast.y).toBeGreaterThanOrEqual(title.y + title.height);
    // The refusal leads to where a provider is added.
    await notice
      .getByRole("button", { name: "Providers", exact: true })
      .click();
    const settings = page.getByRole("dialog", { name: "Settings" });
    await expect(
      settings.getByRole("button", { name: "Add a provider" }).first(),
    ).toBeInViewport();
    await page.keyboard.press("Escape");
    await expect(settings).toHaveCount(0);
    await expect(page.getByLabel("Message or guidance")).toHaveValue("hello");
  });

// A conversation whose runtime started and then had no provider: the host
// says so on the event stream, with its code. Without the code, the same
// words offer nothing: the client keys on the code alone.
const ID = "e".repeat(32);
const SESSION = {
  id: ID,
  generation: "g",
  title: "Refused",
  cwd: "/project",
  status: "idle",
  presence: "active",
  updated: Date.now(),
};
for (const width of [1280, 390])
  devServerTest(
    `a no-provider error on the event stream leads to Providers at ${width}px`,
    async ({ page }) => {
      await page.setViewportSize({ width, height: 844 });
      await page.addInitScript(fakeStream, { epoch: "e", cursor: 1 });
      await mockHost(page, {
        epoch: "e",
        cursor: 1,
        sessions: [SESSION],
        state: { phase: "idle", view: { nodes: [] } },
      });
      await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
      await page.getByLabel("Message or guidance").fill("hello");
      const send = (fields) =>
        page.evaluate(
          (fields) =>
            globalThis.testStream.dispatchEvent(
              new MessageEvent("update", {
                data: JSON.stringify({
                  v: 2,
                  epoch: "e",
                  session_id: "e".repeat(32),
                  generation: "g",
                  kind: "error",
                  ...fields,
                }),
              }),
            ),
          fields,
        );
      const notice = page.locator(".toasts .banner");
      const providers = notice.getByRole("button", {
        name: "Providers",
        exact: true,
      });
      await send({ error: "no provider: add a provider", sequence: 2 });
      await expect(notice).toContainText("no provider");
      await expect(providers).toHaveCount(0);
      await notice.getByRole("button", { name: "Dismiss" }).click();
      await send({
        error: "no provider: add a provider",
        code: "no_provider",
        sequence: 3,
      });
      await providers.click();
      await expect(
        page.getByRole("dialog", { name: "Settings" }),
      ).toBeVisible();
      await page.keyboard.press("Escape");
      await expect(page.getByLabel("Message or guidance")).toHaveValue("hello");
    },
  );

// A real host with no provider refuses to run a new conversation, with its
// code: the toast leads to Providers.
test.describe("a host with no provider", () => {
  test.use({ noProvider: true });

  test("a new conversation's refusal leads to Providers", async ({
    page,
    host,
  }) => {
    await page.goto("/");
    await page.getByRole("button", { name: "New", exact: true }).click();
    await page.getByRole("menuitem", { name: "Folder…" }).click();
    await page.getByRole("button", { name: "Type a path" }).click();
    await page.getByLabel("Folder on the host").fill(host.project);
    await page
      .getByRole("dialog", { name: "New folder" })
      .getByRole("button", { name: "Start conversation", exact: true })
      .click();
    const notice = page.locator(".toasts .banner");
    await expect(notice).toContainText("no provider");
    await notice
      .getByRole("button", { name: "Providers", exact: true })
      .click();
    await expect(
      page
        .getByRole("dialog", { name: "Settings" })
        .getByRole("button", { name: "Add a provider" })
        .first(),
    ).toBeInViewport();
  });
});
