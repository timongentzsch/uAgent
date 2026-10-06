// Sessions created or deleted outside this browser (a coordinator spawning or
// deleting its threads) reach the list on their own, without a refresh.
import { test, expect } from "./fixtures.js";
import { mkdir, rm, writeFile } from "node:fs/promises";

test("the session list follows sessions created and deleted elsewhere", async ({
  page,
  host,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await mkdir(`${host.home}/.uagent/history`, { recursive: true });
  const file = `${host.home}/.uagent/history/elsewhere.json`;
  const messages = [
    { role: "user", content: "Made elsewhere" },
    { role: "assistant", content: "Done" },
  ];
  await writeFile(
    file,
    JSON.stringify({
      format: 3,
      cwd: host.project,
      model: "test",
      session_id: "made-elsewhere",
      title: "Made elsewhere",
      turns: 1,
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
  const row = page.getByText("Made elsewhere", { exact: true }).first();
  await expect(row).toBeVisible();
  await rm(file);
  await expect(row).toHaveCount(0);
});
