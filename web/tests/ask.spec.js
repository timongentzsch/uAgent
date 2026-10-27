// The ask decision: a pending question set from the model, answered in the
// conversation. The snapshot is served with the question in it and the
// reply is recorded, so the page is the real one and no model is needed.
import { test, expect, servePending, SAMPLE_ASK } from "./fixtures.js";

const ASSET_ID = "0123456789abcdef0123456789abcdef";
for (const width of [1440, 900, 390]) {
  test(`ask questions answer with options, Other and an image at ${width}px`, async ({
    page,
    session,
    request,
  }) => {
    await page.setViewportSize({ width, height: 900 });
    await page.route(`**/api/sessions/${session.id}/attachments?*`, (route) =>
      route.fulfill({
        json: { id: ASSET_ID, name: "shot.png", bytes: 68, image: true },
      }),
    );
    await page.route(`**/api/sessions/${session.id}/assets/*`, (route) =>
      route.fulfill({
        contentType: "image/svg+xml",
        body: '<svg xmlns="http://www.w3.org/2000/svg" width="4" height="3"/>',
      }),
    );
    const commands = await servePending(page, request, session, SAMPLE_ASK);
    await page.goto(`/#session=${session.id}`);
    const ask = page.getByRole("region", { name: "Pending decision" });
    // Routed to the panel, the form still answers: the first wins.
    await expect(
      ask.getByRole("heading", { name: "The lead agent is deciding" }),
    ).toBeVisible();
    // One question to a page: Next until the review, which holds Submit.
    const submit = ask.getByRole("button", { name: "Submit" });
    const next = ask.getByRole("button", { name: "Next" });
    const step = (name) => ask.getByRole("tab", { name });
    const contained = () =>
      ask.evaluate((section) =>
        [section, ...section.querySelectorAll("*")].every(
          (node) =>
            node.getBoundingClientRect().right <= innerWidth + 0.5 &&
            node.scrollWidth <= node.clientWidth + 1,
        ),
      );
    await expect(submit).toHaveCount(0);
    await expect(next).toBeDisabled();
    await expect(step(/Storage/)).toHaveAttribute("aria-selected", "true");
    // The steps lead back, never ahead of what has been seen.
    await expect(step(/Platforms/)).toBeDisabled();
    await expect(ask.getByRole("group", { name: /Platforms/i })).toHaveCount(0);

    await ask.getByRole("button", { name: "Cancel" }).click();
    await expect
      .poll(() => commands)
      .toMatchObject([
        { kind: "reply", interaction_id: "ask-sample", cancelled: true },
      ]);

    // Single choice: a tap answers and moves on; Back keeps the answer.
    const storage = ask.getByRole("group", { name: /Storage/i });
    const memory = storage.getByRole("radio", { name: /^Memory/ });
    // A click, not check(): the page has moved on before it could re-read.
    await memory.click();
    const platforms = ask.getByRole("group", { name: /Platforms/i });
    await expect(platforms).toBeVisible();
    await expect(step(/Storage/)).toHaveAccessibleName(/Answered/);
    await ask.getByRole("button", { name: "Back" }).click();
    await expect(memory).toBeChecked();
    // Arrow keys move the choice and stay on the page; Other excludes the
    // options and needs its text.
    // Safari does not focus a clicked radio.
    await memory.focus();
    await page.keyboard.press("ArrowDown");
    await expect(storage.getByRole("radio", { name: /^Disk/ })).toBeChecked();
    await storage.getByRole("radio", { name: /^Other/ }).check();
    await expect(
      storage.getByRole("radio", { name: /^Disk/ }),
    ).not.toBeChecked();
    await expect(next).toBeDisabled();
    const other = storage.getByRole("textbox", {
      name: "Other answer: Storage",
    });
    await other.fill("SQLite");
    // Enter goes on, as Next does.
    await other.press("Enter");

    // Multiple choice, with an image: waits for Next.
    await platforms.getByRole("checkbox", { name: /^macOS/ }).check();
    await platforms.getByRole("checkbox", { name: /^Linux/ }).check();
    await platforms.getByLabel("Attach image: Platforms").setInputFiles({
      name: "shot.png",
      mimeType: "image/png",
      buffer: Buffer.alloc(68),
    });
    await expect(
      platforms.getByRole("button", { name: "Remove shot.png" }),
    ).toBeVisible();
    await expect(platforms).toBeVisible();
    expect(await contained()).toBe(true);
    await next.click();

    // Options the agent showed come with its image and preview; the image's
    // alt text is the option's description.
    const layout = ask.getByRole("group", { name: /Layout/i });
    await expect(
      layout.getByRole("img", {
        name: "Sections listed on the left, one open at a time.",
      }),
    ).toBeVisible();
    await expect(layout.locator(".ask-preview")).toHaveCount(2);
    expect(await contained()).toBe(true);
    await layout.getByRole("radio", { name: /^Tabs/ }).click();

    // The review lists every answer; a row leads back to its question.
    await expect(step("Review")).toHaveAttribute("aria-selected", "true");
    await expect(submit).toBeEnabled();
    await ask.getByRole("button", { name: /^Storage.*SQLite/ }).click();
    await expect(other).toHaveValue("SQLite");
    await step("Review").click();
    await expect(
      ask.getByRole("button", { name: /^Platforms.*macOS, Linux, shot\.png/ }),
    ).toBeVisible();

    // Nothing spills sideways at this width.
    expect(await contained()).toBe(true);

    await submit.click();
    await expect.poll(() => commands.length).toBe(2);
    expect(commands[1]).toMatchObject({
      kind: "reply",
      interaction_id: "ask-sample",
      attachment_ids: [ASSET_ID],
    });
    expect(JSON.parse(commands[1].text)).toEqual([
      { choices: [], other: "SQLite" },
      { choices: ["macOS", "Linux"], other: "", attachment_id: ASSET_ID },
      { choices: ["Tabs"], other: "" },
    ]);
  });
}
