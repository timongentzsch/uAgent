import { expect, selectProbeModel, test } from "./fixtures.js";

// One upload's limit is the host's: `upload_bytes` in its catalogue, which
// is attachments.maxMb (10 MiB unless set) as the host started with it.
const MiB = 1024 * 1024;
const file = (name, size) => ({
  name,
  mimeType: "text/plain",
  buffer: Buffer.alloc(size, "a"),
});
const uploads = (page) => {
  const sent = [];
  page.on("request", (request) => {
    if (/\/attachments\?/.test(request.url())) sent.push(request.url());
  });
  return sent;
};
const notice = (page) => page.locator(".toasts .banner");

test.describe("the composer", () => {
  test.beforeEach(({ command, session }) => selectProbeModel(command, session));

  test("attaches a file the host's limit holds, past the old 8 MiB", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    const sent = uploads(page);
    await page
      .locator('.composer input[type="file"]')
      .setInputFiles(file("nine.txt", 9 * MiB));
    const chip = page.locator(".composer .file-chip");
    await expect(chip).toContainText("nine.txt");
    await expect(chip.locator("[aria-busy=true], .spinner")).toHaveCount(0);
    expect(sent).toHaveLength(1);
    await expect(notice(page)).toHaveCount(0);
  });

  test("refuses a file over the host's limit before sending it, and says the limit", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    const sent = uploads(page);
    await page
      .locator('.composer input[type="file"]')
      .setInputFiles(file("eleven.txt", 11 * MiB));
    await expect(notice(page)).toContainText("at most 10 MiB each");
    await expect(page.locator(".composer .file-chip")).toHaveCount(0);
    expect(sent).toEqual([]);
  });

  test("says a file the host refused by its size is too large, never that the host is out of reach", async ({
    page,
    session,
  }) => {
    await page.route(/\/attachments\?/, (route) =>
      route.fulfill({ status: 413, json: { error: "Payload Too Large" } }),
    );
    await page.goto(`/#session=${session.id}`);
    await page
      .locator('.composer input[type="file"]')
      .setInputFiles(file("nine.txt", 9 * MiB));
    await expect(notice(page)).toContainText("nine.txt is too large to attach");
    await expect(page.locator(".composer .file-chip")).toHaveCount(0);
  });
});

test.describe("the home screen's start box", () => {
  test.use({ places: true });

  test("takes a file the host's limit holds, and refuses one over it before sending", async ({
    page,
  }) => {
    await page.goto("/");
    const sent = uploads(page);
    const box = page.locator(".start-field");
    await expect(page.getByLabel("Start a conversation")).toBeEnabled();
    await box
      .getByLabel("Attach files")
      .setInputFiles(file("eleven.txt", 11 * MiB));
    await expect(notice(page)).toContainText("at most 10 MiB each");
    await expect(box.locator(".file-chip")).toHaveCount(0);
    await box
      .getByLabel("Attach files")
      .setInputFiles(file("nine.txt", 9 * MiB));
    await expect(box.locator(".file-chip")).toContainText("nine.txt");
    await page.getByLabel("Start a conversation").fill("Read this");
    // The model the host's scripted provider answers as (tests/web_host.py).
    await page
      .getByRole("button", { name: "Model for this conversation", exact: true })
      .click();
    await page.getByLabel("Model", { exact: true }).fill("model-b");
    await page.getByRole("option").filter({ hasText: "mock/model-b" }).click();
    await page.getByRole("button", { name: "Apply", exact: true }).click();
    await page.getByRole("button", { name: "Start", exact: true }).click();
    const first = page.locator(".transcript .message.user").first();
    await expect(first).toContainText("nine.txt");
    expect(sent).toHaveLength(1);
  });
});
