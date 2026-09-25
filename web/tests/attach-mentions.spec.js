// Named attachments + @-mentions: deduped labels, composer rename, inline
// token references, and no raw host paths in the transcript.
import { test, expect } from "./fixtures.js";

const pixel = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==",
  "base64",
);

async function ready(page, session, command) {
  await page.goto(`/#session=${session.id}`);
  await expect(page.getByLabel("Message or guidance")).toBeVisible();
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
}

async function attach(page, name) {
  await page
    .locator('.composer input[type="file"]')
    .setInputFiles({ name, mimeType: "image/png", buffer: pixel });
  await expect(
    page.locator(".composer .file-chip", { hasText: name }),
  ).toBeVisible();
}

test("duplicate uploads dedupe, rename and @-mention send cleanly", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  await ready(page, session, command);
  await attach(page, "image.png");
  await attach(page, "image.png");
  await expect(
    page.locator(".composer .file-chip", { hasText: "image (2).png" }),
  ).toBeVisible();

  await page.getByRole("button", { name: "Rename image.png" }).click();
  const rename = page.getByLabel("Rename image.png");
  await rename.fill("chart");
  await rename.press("Enter");
  await expect(
    page.locator(".composer .file-chip", { hasText: "chart" }),
  ).toBeVisible();

  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("look @ch");
  await expect(
    page.getByRole("listbox", { name: "Attached files" }),
  ).toBeVisible();
  await prompt.press("ArrowDown");
  await prompt.press("Enter");
  await expect(prompt).toHaveValue(/!\[chart\]\(attachment:[A-Za-z0-9_-]+\)/);
  await prompt.press("Enter");

  const transcript = page.locator(".transcript");
  // Tiles carry the renamed label; the server path trailer never shows.
  await expect(
    transcript.getByRole("button", { name: /^View chart/ }).first(),
  ).toBeVisible({ timeout: 30000 });
  expect(await transcript.textContent()).not.toContain('path "');
  await expect(transcript.locator(".mention-image img")).toHaveCount(1);
});

test("removed attachment degrades the token instead of breaking", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  await ready(page, session, command);
  await attach(page, "shot.png");
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("see @sh");
  await page.getByRole("option", { name: "@shot.png" }).click();
  await page.getByRole("button", { name: "Remove shot.png" }).click();
  await prompt.press("Enter");

  const transcript = page.locator(".transcript");
  await expect(transcript.getByText("attachment removed").first()).toBeVisible({
    timeout: 30000,
  });
  expect(await transcript.textContent()).not.toContain('path "');
});

test("steer carries files when idle converts to a turn", async ({
  page,
  session,
  command,
  request,
  host,
}) => {
  test.setTimeout(120000);
  await ready(page, session, command);
  const upload = await request.post(
    `/api/sessions/${session.id}/attachments?name=steer.png`,
    {
      headers: { Origin: host.origin, "Content-Type": "image/png" },
      data: pixel,
    },
  );
  expect(upload.ok(), await upload.text()).toBe(true);
  const asset = await upload.json();
  const result = await command("steer", {
    session_id: session.id,
    generation: session.generation,
    text: "describe the attached",
    attachment_ids: [{ id: asset.id, name: "steer.png" }],
  });
  expect(JSON.stringify(result)).not.toMatch(
    /invalid asset|unavailable|too many|invalid attachment name|guidance requires/,
  );
  const transcript = page.locator(".transcript");
  await expect(
    transcript.getByRole("button", { name: "View steer.png" }).first(),
  ).toBeVisible({ timeout: 60000 });
  expect(await transcript.textContent()).not.toContain('path "');
});
