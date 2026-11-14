// A host with no provider offers OpenRouter's sign-in on the home screen.
import { test, expect } from "./fixtures.js";

// Answers the host's provider and sign-in commands; `asked` holds the latter.
async function withoutProvider(page, configured = false) {
  const asked = [];
  await page.route("**/api/command", (route) => {
    const body = route.request().postDataJSON();
    const reply = (result) =>
      route.fulfill({
        json: { request_id: body.request_id, accepted: true, result },
      });
    if (body.kind === "provider") return reply({ configured });
    if (body.kind !== "openrouter") return route.continue();
    asked.push(body);
    return reply(
      body.action === "start"
        ? { url: "https://openrouter.example/auth?code_challenge=abc" }
        : { model: "openrouter-free/coder:free" },
    );
  });
  return asked;
}

test("without a provider, home signs in to OpenRouter with a pasted code", async ({
  page,
  host,
}) => {
  const asked = await withoutProvider(page);
  await page.goto("/");
  const card = page.getByRole("region", { name: "Start with a free model" });
  await card.getByRole("button", { name: "Sign in with OpenRouter" }).click();
  // This address is not one OpenRouter returns to, so it shows a code.
  await expect(
    card.getByRole("link", { name: "Open OpenRouter and approve" }),
  ).toHaveAttribute(
    "href",
    "https://openrouter.example/auth?code_challenge=abc",
  );
  expect(asked[0]).toMatchObject({ action: "start", callback: "" });
  const finish = card.getByRole("button", { name: "Finish sign-in" });
  await expect(finish).toBeDisabled();
  await card.getByLabel("Code from OpenRouter").fill("the-code");
  await finish.click();
  await expect(
    page.getByText("New conversations use openrouter-free/coder:free"),
  ).toBeVisible();
  expect(asked[1]).toMatchObject({ action: "finish", code: "the-code" });
  await expect(card).toHaveCount(0);
});

test("a return from OpenRouter redeems the code and leaves the address clean", async ({
  page,
  host,
}) => {
  const asked = await withoutProvider(page);
  await page.goto("/?openrouter=1&code=returned&state=mine");
  await expect(
    page.getByText("New conversations use openrouter-free/coder:free"),
  ).toBeVisible();
  expect(asked).toHaveLength(1);
  expect(asked[0]).toMatchObject({
    action: "finish",
    code: "returned",
    state: "mine",
  });
  expect(new URL(page.url()).search).toBe("");
});

test("a host with a provider offers no sign-in", async ({ page, host }) => {
  await withoutProvider(page, true);
  await page.goto("/");
  await expect(page.getByLabel("Start a conversation")).toBeVisible();
  await expect(
    page.getByRole("region", { name: "Start with a free model" }),
  ).toHaveCount(0);
});
