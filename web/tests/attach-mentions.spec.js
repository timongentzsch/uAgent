// Named attachments + @-mentions: deduped labels, composer rename, inline
// token references, and no raw host paths in the transcript.
import {
  test,
  expect,
  online,
  quotesOf,
  selectProbeModel,
  writeFiles,
  withoutServiceWorker,
} from "./fixtures.js";

const pixel = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==",
  "base64",
);

async function ready(page, session, command) {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await selectProbeModel(command, session, { yolo: true });
}

async function attach(page, name) {
  await page
    .locator('.composer input[type="file"]')
    .setInputFiles({ name, mimeType: "image/png", buffer: pixel });
  await expect(
    page.locator(".composer .file-chip", { hasText: name }),
  ).toBeVisible();
  // Uploaded, not merely listed: nothing is sent while one is under way.
  await expect(page.locator(".composer [aria-busy]")).toHaveCount(0);
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
    page.getByRole("listbox", { name: "File mentions" }),
  ).toBeVisible();
  await prompt.press("ArrowDown");
  await prompt.press("Enter");
  await expect(prompt).toHaveValue(/!\[chart\]\(attachment:[A-Za-z0-9_-]+\)/);
  await prompt.press("Enter");

  const transcript = page.locator(".transcript");
  await expect(
    transcript.getByRole("button", { name: /^View chart/ }).first(),
  ).toBeVisible({ timeout: 30000 });
  expect(await transcript.textContent()).not.toContain('path "');
  await expect(transcript.locator(".mention-image img")).toHaveCount(1);
});

test("workspace references keep full paths and optional lines through reload and send", async ({
  page,
  session,
  host,
  command,
}) => {
  await writeFiles(host.project, {
    "api/sample.ts": "first content marker",
    "cli/sample.ts": "second content marker",
  });
  await ready(page, session, command);
  const prompt = page.getByLabel("Message or guidance");
  const quote = page.locator(".composer .quote-bar");
  const paths = page.getByRole("group", { name: "Workspace references" });
  await prompt.fill("/btw Review @sample");
  await paths.getByRole("option", { name: "api/sample.ts" }).click();
  await expect(prompt).toHaveValue("/btw Review `api/sample.ts` ");
  await expect(quote).toHaveCount(0);
  await prompt.fill("Review @sample");
  await expect(paths.getByRole("option")).toHaveCount(2);
  await paths.getByRole("option", { name: "api/sample.ts" }).click();
  await expect(quote).toContainText("api/sample.ts");
  await quote.getByRole("button", { name: "Remove the quote" }).click();
  await expect(quote).toHaveCount(0);
  for (const suffix of ["#L2-L4", ":2-4"]) {
    await prompt.fill("Review @sample" + suffix);
    await paths.getByRole("option", { name: "cli/sample.ts:2-4" }).click();
  }
  await expect(quote).toHaveCount(1);
  await expect
    .poll(() => quotesOf(page, session.id))
    .toEqual(["`cli/sample.ts:2-4`"]);
  await page.reload();
  await online(page);
  await expect(quote).toContainText("cli/sample.ts:2-4");
  await expect(prompt).toHaveValue("Review ");
  const submitted = page.waitForRequest(
    (request) =>
      request.url().endsWith("/api/command") &&
      request.postDataJSON()?.kind === "submit",
  );
  await prompt.press("Enter");
  const { text, attachment_ids } = (await submitted).postDataJSON();
  expect(text.trimEnd()).toBe("`cli/sample.ts:2-4`\n\nReview");
  expect(attachment_ids ?? []).toEqual([]);
  await expect(quote).toHaveCount(0);
  await expect(
    page
      .locator(".transcript .file-ref")
      .filter({ hasText: "cli/sample.ts:2-4" })
      .first(),
  ).toBeVisible();
});

test("editing a mention immediately drops old paths and Escape dismisses a pending search", async ({
  page,
  session,
  host,
}) => {
  await withoutServiceWorker(page);
  await writeFiles(host.project, { "old-name.ts": "", "new-name.ts": "" });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("@old-name");
  const option = page.getByRole("option", { name: "old-name.ts" });
  await expect(option).toBeVisible();
  let release;
  const blocked = new Promise((resolve) => {
    release = resolve;
  });
  await page.route(
    `**/api/sessions/${session.id}/files?find=new-name`,
    async (route) => {
      await blocked;
      await route
        .fulfill({ json: { paths: ["new-name.ts"], more: false } })
        .catch(() => {});
    },
  );
  const searching = page.waitForRequest((request) =>
    request.url().endsWith("?find=new-name"),
  );
  await prompt.fill("@new-name");
  try {
    await searching;
    await expect(option).toHaveCount(0);
    await prompt.press("Escape");
  } finally {
    release();
  }
  await prompt.press("Tab");
  await expect(prompt).toHaveValue("@new-name");
  await expect(
    page.getByRole("listbox", { name: "File mentions" }),
  ).toHaveCount(0);
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

test("pasting text with a rendered image leaves it to the browser, no attachment", async ({
  page,
  session,
  command,
}) => {
  await ready(page, session, command);
  const prompt = page.getByLabel("Message or guidance");
  await prompt.focus();
  // What a spreadsheet puts on the clipboard: the cells as text beside a
  // picture of them. The browser inserts the text itself.
  const kept = await prompt.evaluate(
    (element, bytes) => {
      const data = new DataTransfer();
      data.setData("text/plain", "a\tb");
      data.items.add(
        new File([new Uint8Array(bytes)], "cells.png", { type: "image/png" }),
      );
      return element.dispatchEvent(
        new ClipboardEvent("paste", {
          clipboardData: data,
          bubbles: true,
          cancelable: true,
        }),
      );
    },
    [...pixel],
  );
  expect(kept).toBe(true);
  await expect(page.locator(".composer .file-chip")).toHaveCount(0);
});

test("a file dropped on the transcript attaches instead of navigating", async ({
  page,
  session,
  command,
  context,
}) => {
  await ready(page, session, command);
  // A dragover left uncancelled is what lets the browser open the file.
  const drop = () =>
    page.locator(".transcript").evaluate(
      (element, bytes) => {
        const data = new DataTransfer();
        data.items.add(
          new File([new Uint8Array(bytes)], "dropped.png", {
            type: "image/png",
          }),
        );
        const [over] = ["dragover", "drop"].map((type) =>
          element.dispatchEvent(
            new DragEvent(type, {
              dataTransfer: data,
              bubbles: true,
              cancelable: true,
            }),
          ),
        );
        return over;
      },
      [...pixel],
    );
  expect(await drop()).toBe(false);
  const chips = page.locator(".composer .file-chip", {
    hasText: "dropped.png",
  });
  await expect(chips).toHaveCount(1);
  await context.setOffline(true);
  await expect(
    page.getByRole("button", { name: "Send", exact: true }),
  ).toBeDisabled();
  await drop();
  await expect(
    page.getByText("Not connected: nothing was attached"),
  ).toBeVisible();
  await expect(chips).toHaveCount(1);
});

test("a draft reloads without an interrupted upload", async ({
  page,
  session,
  command,
}) => {
  await ready(page, session, command);
  await page.evaluate((id) => {
    localStorage.setItem(
      `uagent-drafts:${id}`,
      JSON.stringify({
        text: "kept",
        files: [
          { id: "upload-lost", name: "lost.png", bytes: 1, pending: true },
        ],
        quotes: [],
      }),
    );
  }, session.id);
  await page.reload();
  await expect(page.getByLabel("Message or guidance")).toHaveValue("kept");
  await expect(page.locator(".composer .file-chip")).toHaveCount(0);
});
