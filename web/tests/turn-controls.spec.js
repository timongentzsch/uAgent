// A turn's controls: deciding an approval, queueing the next message,
// stopping and continuing, retrying a send that failed, and undoing the
// files a turn changed.
import { test, expect } from "./fixtures.js";
import { access, realpath } from "node:fs/promises";

// The mock provider answers as model-b.
test.beforeEach(({ command, session }) =>
  command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  }),
);

const exists = (path) =>
  access(path).then(
    () => true,
    () => false,
  );

async function send(page, text) {
  await page.getByLabel("Message or guidance").fill(text);
  await page.getByRole("button", { name: "Send", exact: true }).click();
}

test("always allowing an action remembers it in Settings", async ({
  page,
  session,
  host,
}) => {
  await page.goto(`/#session=${session.id}`);
  await send(page, "request approval");
  const card = page.getByRole("region", { name: "Pending decision" });
  await expect(card).toContainText("browser-proof.txt");
  // The host names the folder by its real path (/private/var on macOS).
  await expect(card).toContainText(`in ${await realpath(host.project)}`);
  await expect(card.locator(".risk-chip").first()).toBeVisible();
  await expect(
    card.getByRole("button", { name: "Allow for session" }),
  ).toBeVisible();
  // One row of answers; the rest waits under More options.
  const answers = await card
    .locator(".decision-answers > button")
    .evaluateAll((buttons) =>
      buttons.map((button) => {
        const { y, width, height } = button.getBoundingClientRect();
        return { name: button.textContent, y, width, height };
      }),
    );
  expect(answers.map((answer) => answer.name)).toEqual([
    "Deny",
    "Allow for session",
    "Allow once",
  ]);
  for (const answer of answers) {
    expect(Math.abs(answer.y - answers[0].y)).toBeLessThanOrEqual(1);
    expect(Math.abs(answer.width - answers[0].width)).toBeLessThanOrEqual(1);
  }
  await expect(
    card.getByRole("checkbox", { name: "Always allow this exact action here" }),
  ).toBeHidden();
  await card.getByText("More options").click();
  // Guidance stays folded until asked for.
  await expect(card.getByLabel("Guidance")).toHaveCount(0);
  await card.getByRole("button", { name: "+ guidance" }).click();
  await expect(card.getByLabel("Guidance")).toBeVisible();
  await card
    .getByRole("checkbox", { name: "Always allow this exact action here" })
    .check();
  await card.getByRole("button", { name: "Allow once", exact: true }).click();
  await expect(card).toHaveCount(0);
  await expect
    .poll(() => exists(`${host.project}/browser-proof.txt`))
    .toBe(true);
  await expect(page.locator(".composer .activity-caption")).toHaveText(
    "Ready",
    { timeout: 15000 },
  );
  await send(page, "/permissions");
  const settings = page.getByRole("dialog", { name: "Settings" });
  // What this repository remembers is among the project's settings.
  await settings
    .locator(".settings-nav")
    .getByRole("button", { name: /^This project/ })
    .click();
  await expect(settings.getByLabel(/^Allowed actions/)).toContainText(
    "write_file",
  );
});

test("an undone turn puts its file back", async ({ page, session, host }) => {
  await page.goto(`/#session=${session.id}`);
  await send(page, "request approval");
  await page.getByRole("button", { name: "Allow once", exact: true }).click();
  const receipt = page.getByRole("button", { name: /^Review changes: / });
  await expect(receipt).toHaveText(/1 file · \+1 −0/, { timeout: 15000 });
  await expect(
    page.getByRole("button", { name: /^Turn statistics: / }),
  ).toBeVisible();
  expect(await exists(`${host.project}/browser-proof.txt`)).toBe(true);
  await receipt.click();
  const sheet = page.getByRole("dialog", { name: "Changed files" });
  await expect(sheet).toContainText("Shell changes aren't tracked");
  await sheet.getByRole("button", { name: "Undo browser-proof.txt" }).click();
  await expect(sheet).toContainText("Restored");
  expect(await exists(`${host.project}/browser-proof.txt`)).toBe(false);
  await expect(sheet.getByRole("button", { name: "Undo all" })).toBeDisabled();
});

test("a message queued during a turn runs after it", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await send(page, "Long continuity probe");
  const prompt = page.getByLabel("Message or guidance");
  await expect(prompt).toHaveAttribute(
    "placeholder",
    "Add guidance… (Esc to stop)",
  );
  // The status line's slot offers Queue next only once there is a draft,
  // and the primary turns from Stop into Send guidance in place.
  const queue = page.getByRole("button", { name: "Queue next" });
  await expect(queue).toBeHidden();
  await expect(page.getByRole("button", { name: "Stop" })).toBeVisible();
  // Its row in the list shows work with the one breathing LED, no icon.
  const row = page.locator(".session-row:has([aria-current])");
  await expect(row.locator(".status-led.running")).toBeVisible();
  await expect(row.locator(".session-state")).toHaveCount(0);
  await prompt.fill("Queued follow-up");
  await expect(page.getByRole("button", { name: "Stop" })).toHaveCount(0);
  await expect(
    page.getByRole("button", { name: "Send guidance", exact: true }),
  ).toBeEnabled();
  await queue.click();
  await expect(
    page.locator(".message.user", { hasText: "Queued follow-up" }),
  ).toContainText("Queued for after this turn");
  await expect(
    page.getByRole("button", { name: /^Turn statistics: / }),
  ).toHaveCount(2, { timeout: 30000 });
  await expect(page.getByText("Queued for after this turn")).toHaveCount(0);
});

test("a stopped turn ends with Continue", async ({ page, session }) => {
  await page.goto(`/#session=${session.id}`);
  await send(page, "Long continuity probe");
  await expect(page.getByRole("button", { name: "Stop" })).toBeVisible();
  await page.getByLabel("Message or guidance").press("Escape");
  // The status line above the input says so and offers the way on.
  const line = page.locator(".composer .status-line");
  await expect(line.locator(".activity-caption")).toHaveText("Stopped", {
    timeout: 15000,
  });
  await expect(line.getByRole("status")).toHaveText("Stopped");
  const resume = line.getByRole("button", { name: "Continue" });
  await resume.click();
  await expect(page.locator(".message.user").last()).toContainText("continue");
  await expect(resume).toHaveCount(0);
});

test("a refused send stays at its message with Retry", async ({
  page,
  session,
}) => {
  let refused = false;
  await page.route("**/api/command", (route) => {
    const body = route.request().postDataJSON();
    if (body.kind !== "submit" || refused) return route.continue();
    refused = true;
    return route.fulfill({
      contentType: "application/json",
      body: JSON.stringify({
        request_id: body.request_id,
        accepted: false,
        error: "Refused for this test",
      }),
    });
  });
  await page.goto(`/#session=${session.id}`);
  await send(page, "Retry probe");
  const row = page.locator(".message.user", { hasText: "Retry probe" });
  await expect(row.getByRole("alert")).toContainText("Refused for this test");
  await expect(page.locator(".error-banner")).toHaveCount(0);
  await row.getByRole("button", { name: "Retry" }).click();
  await expect(
    page.getByRole("heading", { name: "Verified response" }),
  ).toBeVisible({ timeout: 15000 });
  await expect(page.getByText("Refused for this test")).toHaveCount(0);
});

// The input frame is one shape whatever the turn is doing: idle, running,
// stopped and waiting on a decision draw the textarea, the frame and the
// permission chip in the same place, so nothing under a thumb moves.
for (const [name, viewport] of [
  ["phone", { width: 390, height: 844 }],
  ["desktop", { width: 1280, height: 800 }],
]) {
  test(`the composer frame holds still across turn states (${name})`, async ({
    page,
    session,
  }) => {
    await page.setViewportSize(viewport);
    await page.goto(`/#session=${session.id}`);
    const prompt = page.getByLabel("Message or guidance");
    await expect(prompt).toBeEnabled();
    const measure = () =>
      page.evaluate(() => {
        const box = (selector) => {
          const node = document.querySelector(selector);
          if (!node) return null;
          const { x, y, width, height } = node.getBoundingClientRect();
          return { x, y, width, height };
        };
        return {
          textarea: box("#prompt"),
          frame: box(".composer > form"),
          permission: box(".permission-control > button"),
        };
      });
    const states = { idle: await measure() };
    await send(page, "Long continuity probe");
    await expect(page.getByRole("button", { name: "Stop" })).toBeVisible();
    states.running = await measure();
    await prompt.press("Escape");
    await expect(page.getByRole("button", { name: "Continue" })).toBeVisible({
      timeout: 15000,
    });
    states.stopped = await measure();
    await send(page, "request approval");
    await expect(
      page.getByRole("region", { name: "Pending decision" }),
    ).toBeVisible();
    states.approval = await measure();
    await test.info().attach("composer-geometry.json", {
      body: JSON.stringify(states, null, 2),
      contentType: "application/json",
    });
    for (const state of ["running", "stopped", "approval"])
      for (const part of ["textarea", "frame", "permission"]) {
        const actual = states[state][part];
        const expected = states.idle[part];
        expect(actual, `${state}: ${part}`).not.toBeNull();
        for (const key of ["x", "y", "width", "height"])
          expect(
            Math.abs(actual[key] - expected[key]),
            `${state}: ${part}.${key} ${JSON.stringify(actual)} vs idle ${JSON.stringify(expected)}`,
          ).toBeLessThanOrEqual(1);
      }
    // Deciding hands the keyboard back to the input it waited above.
    await page.getByRole("button", { name: "Allow once", exact: true }).click();
    await expect(prompt).toBeFocused();
  });
}
