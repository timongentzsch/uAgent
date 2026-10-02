// Rewinding never loses work: it continues in a fork cut before the message,
// with that message back in the composer, and the original stays.
import { test, expect } from "./fixtures.js";
import { mkdir, writeFile } from "node:fs/promises";

test("edit from a message continues in a fork with the message to edit", async ({
  page,
  host,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await mkdir(`${host.home}/.uagent/history`, { recursive: true });
  const messages = [
    { role: "user", content: "First request" },
    { role: "assistant", content: "First answer" },
    { role: "user", content: "Second request to edit" },
    { role: "assistant", content: "Second answer" },
  ];
  await writeFile(
    `${host.home}/.uagent/history/rewind.json`,
    JSON.stringify({
      format: 3,
      cwd: host.project,
      model: "test",
      session_id: "rewind-source",
      title: "Rewind source",
      turns: 2,
    }) +
      "\n" +
      JSON.stringify({
        messages,
        message_kinds: messages.map((message) => message.role),
        archive: [],
        archive_dropped_segments: 0,
        context_tokens: 0,
        usage: {},
        tool_displays: {},
      }),
  );
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
