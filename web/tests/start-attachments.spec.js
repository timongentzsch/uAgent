import { expect, test } from "./fixtures.js";

// Files attached on the home screen go with the conversation's first message.
test.use({ places: true });

const PNG = {
  name: "sample.png",
  mimeType: "image/png",
  buffer: Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAAAAACRn/EaAAAADklEQVR4nGNoAAIGOAEALQ8GAa0C414AAAAASUVORK5CYII=",
    "base64",
  ),
};
const NOTE = {
  name: "note.txt",
  mimeType: "text/plain",
  buffer: Buffer.from("aside"),
};

async function attachOnHome(page) {
  await page.goto("/");
  const field = page.getByLabel("Start a conversation");
  await expect(field).toBeEnabled();
  const box = page.locator(".start-field");
  await box.getByLabel("Attach files").setInputFiles([PNG, NOTE]);
  await expect(box.locator(".file-chip")).toHaveCount(2);
  await box.getByRole("button", { name: "Remove note.txt" }).click();
  await expect(box.locator(".file-chip")).toHaveText(/sample\.png/);
  // The attach control takes no press meant for the controls beside it.
  await box
    .getByRole("button", { name: "Scratch", exact: true })
    .click({ trial: true });
  await field.fill("What is in this picture?");
}

test("a picture attached on the home screen goes with the first message", async ({
  page,
}) => {
  await attachOnHome(page);
  // The model the host's scripted provider answers as (tests/web_host.py).
  await page
    .getByRole("button", { name: "Model for this conversation", exact: true })
    .click();
  await page.getByLabel("Model", { exact: true }).fill("model-b");
  await page.getByRole("option").filter({ hasText: "mock/model-b" }).click();
  await page.getByRole("button", { name: "Apply", exact: true }).click();
  await page.getByRole("button", { name: "Start", exact: true }).click();
  const first = page.locator(".transcript .message.user").first();
  await expect(first).toContainText("What is in this picture?");
  await expect(
    first.getByRole("button", { name: "View sample.png" }),
  ).toBeVisible();
  await expect(first.getByText("note.txt")).toHaveCount(0);
  await expect(page.locator(".composer .file-chip")).toHaveCount(0);
  await expect(page.getByLabel("Message or guidance")).toHaveValue("");
  await page
    .getByRole("button", { name: "Model and effort", exact: true })
    .click({ trial: true });
});

test("a first message the host refuses stays in the new conversation's draft", async ({
  page,
}) => {
  await page.route("**/api/command", (route) =>
    route.request().postDataJSON()?.kind === "submit"
      ? route.fulfill({ status: 503, json: { error: "Host busy" } })
      : route.continue(),
  );
  await attachOnHome(page);
  await page.getByRole("button", { name: "Start", exact: true }).click();
  await expect(page.getByText("Host busy")).toBeVisible();
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "What is in this picture?",
  );
  await expect(page.locator(".composer .file-chip")).toHaveText(/sample\.png/);
  await expect(page.locator(".transcript .message.user")).toHaveCount(0);
});
