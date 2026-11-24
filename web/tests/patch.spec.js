// A patch changes several files in one call; each shows as its own change.
import { test, expect, online, openRow, selectProbeModel } from "./fixtures.js";

test("a patch shows each file it changed, and still does after a reload", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.getByLabel("Message or guidance").fill("Patch probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  const transcript = page.locator(".transcript");
  await expect(transcript).toContainText("Native worker");
  const shown = async () => {
    await openRow(transcript, "Worked");
    await openRow(transcript, "Patched 2 files");
    for (const name of ["patched-one.txt", "patched-two.txt"])
      await expect(
        transcript.locator(".node-row", { hasText: name }),
      ).toHaveCount(1);
    await expect(
      transcript.locator(".node-row", { hasText: "apply_patch" }),
    ).toHaveCount(0);
  };
  await shown();
  await page.reload();
  await online(page);
  await shown();
});
