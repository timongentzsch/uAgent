// A turn's controls: deciding an approval, queueing the next message,
// stopping and continuing, retrying a send that failed, and undoing the
// files a turn changed.
import { test, expect } from "./fixtures.js";
import { access } from "node:fs/promises";

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
  await expect(card).toContainText(`in ${host.project}`);
  await expect(card.locator(".risk-chip").first()).toBeVisible();
  await expect(
    card.getByRole("button", { name: "Allow for session" }),
  ).toBeVisible();
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
  await prompt.fill("Queued follow-up");
  await page.getByRole("button", { name: "Queue next" }).click();
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
  const chip = page.locator(".stop-chip");
  await expect(chip).toContainText("Stopped · Continue", { timeout: 15000 });
  await chip.getByRole("button", { name: "Continue" }).click();
  await expect(page.locator(".message.user").last()).toContainText("continue");
  await expect(chip).toHaveCount(0);
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
