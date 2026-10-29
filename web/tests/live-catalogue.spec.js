// Sessions created or deleted outside this browser (a conversation starting or
// deleting its agents) reach the list on their own, without a refresh.
import { test, expect, saveHistory } from "./fixtures.js";
import { rm } from "node:fs/promises";

test("the session list follows sessions created and deleted elsewhere", async ({
  page,
  host,
}) => {
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  const messages = [
    { role: "user", content: "Made elsewhere" },
    { role: "assistant", content: "Done" },
  ];
  const file = await saveHistory(host, "elsewhere.json", {
    id: "made-elsewhere",
    title: "Made elsewhere",
    turns: 1,
    messages,
  });
  const row = page.getByText("Made elsewhere", { exact: true }).first();
  await expect(row).toBeVisible();
  await rm(file);
  await expect(row).toHaveCount(0);
});
