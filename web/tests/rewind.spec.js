// Rewinding never loses work: it continues in a fork cut before the message,
// with that message back in the composer, and the original stays.
import { test, expect, saveHistory } from "./fixtures.js";

test("edit from a message continues in a fork with the message to edit", async ({
  page,
  host,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  const messages = [
    { role: "user", content: "First request" },
    { role: "assistant", content: "First answer" },
    { role: "user", content: "Second request to edit" },
    { role: "assistant", content: "Second answer" },
  ];
  await saveHistory(host, "rewind.json", {
    id: "rewind-source",
    title: "Rewind source",
    turns: 2,
    messages,
  });
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await page.getByRole("button", { name: /Rewind source/ }).click();
  const second = page
    .locator(".message.user")
    .filter({ hasText: "Second request to edit" });
  await second.getByRole("button", { name: /^Actions for / }).click();
  await page.getByRole("menuitem", { name: "Edit from here" }).click();

  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "Second request to edit",
  );
  const transcript = page.locator(".transcript");
  await expect(transcript).toContainText("First answer");
  await expect(transcript).not.toContainText("Second answer");
  await expect(page.locator(".conversation-head h1")).toContainText("Fork of");
  // The original conversation is untouched and still listed.
  await expect(
    page.getByRole("button", { name: /^Rewind source/ }),
  ).toBeVisible();
});
