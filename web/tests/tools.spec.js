import { test, expect } from "./fixtures.js";

test("tool catalogue reports exact schema bytes and persists selection", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 1200, height: 800 });
  await page.goto(`/#session=${session.id}`);
  await page.getByRole("button", { name: "Conversation menu" }).last().click();
  await page.getByRole("menuitem", { name: "Tools", exact: true }).click();

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

  await dialog.getByText("Categories", { exact: true }).click();
  await dialog.getByLabel("New tool category").fill("Repository tools");
  await dialog.getByRole("button", { name: "Add", exact: true }).click();
  const category = choice.getByRole("combobox");
  await category.selectOption({ label: "Repository tools" });
  await expect(
    dialog.getByRole("heading", { name: "Repository tools" }),
  ).toBeVisible();
  await dialog.getByLabel("Name for Repository tools").fill("Project work");
  await dialog.getByRole("button", { name: "Rename", exact: true }).click();
  await expect(
    dialog.getByRole("heading", { name: "Project work" }),
  ).toBeVisible();
});

test("a shared file previews inline in the conversation, sandboxed", async ({
  page,
  session,
  command,
}) => {
  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
  await command("permissions", {
    session_id: session.id,
    generation: session.generation,
    mode: "yolo",
  });
  await page.goto(`/#session=${session.id}`);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Artifact probe");
  await prompt.press("Enter");
  // The write shows its diff and the share its preview, neither expanded.
  await expect(page.locator(".tool-inline .diff").first()).toContainText(
    "ARTIFACT_REPORT",
  );
  const file = page.locator(".tool-inline .tool-file");
  await expect(file).toContainText("report.html");
  const frame = file.locator("iframe");
  await expect(frame).toHaveAttribute(
    "sandbox",
    "allow-scripts allow-forms allow-popups",
  );
  await expect(
    page.frameLocator(".tool-file iframe").getByText("ARTIFACT_REPORT"),
  ).toBeVisible();
  // An opaque origin: the file's scripts can never read this device's session.
  const preview = page.frames().find((item) => item.url().includes("/assets/"));
  expect(
    await preview.evaluate(() => {
      try {
        return document.cookie;
      } catch {
        return "blocked";
      }
    }),
  ).toBe("blocked");
  await expect(file.getByRole("link", { name: "Open" })).toHaveAttribute(
    "target",
    "_blank",
  );
  await expect(file.getByRole("link", { name: "Download" })).toHaveAttribute(
    "href",
    /\?download=1$/,
  );
});

test("files read as tiles and cards, tool images sit on their row, and every image opens the viewer", async ({
  page,
  session,
  command,
}) => {
  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
  await command("permissions", {
    session_id: session.id,
    generation: session.generation,
    mode: "yolo",
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}`);
  const pixel = Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==",
    "base64",
  );
  await page.locator('input[type="file"]').setInputFiles([
    { name: "layout.png", mimeType: "image/png", buffer: pixel },
    {
      name: "spec.pdf",
      mimeType: "application/pdf",
      buffer: Buffer.from("%PDF-1.4"),
    },
  ]);
  // Before sending: one strip, never a second line.
  const strip = page.locator(".composer .attachments");
  await expect(strip.locator(".file-chip")).toHaveCount(2);
  expect(await strip.evaluate((node) => getComputedStyle(node).flexWrap)).toBe(
    "nowrap",
  );
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Image probe");
  await prompt.press("Enter");
  // Sent: the image as a tile, the PDF as a card naming its type.
  const sent = page.locator(".message.user .attachment-list");
  await expect(sent.locator(".attachment-tile")).toHaveCount(1);
  await expect(sent.locator(".attachment-card")).toContainText("PDF");
  // The image read_path added to context sits on that call's row.
  const read = page.locator(".message.tool").filter({ hasText: "shot.png" });
  await expect(read.locator(".attachment-tile")).toHaveCount(1);
  await expect(
    page.locator(".message").filter({ hasText: "attached on request" }),
  ).toHaveCount(0);
  await read.locator(".attachment-tile").click();
  const viewer = page.getByRole("dialog", { name: "shot.png" });
  await expect(viewer.locator("img")).toBeVisible();
  await viewer.locator("img").click();
  await expect(viewer).toHaveCount(0);
});
