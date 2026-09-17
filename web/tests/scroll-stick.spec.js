// Follow-tail guarantees under live streaming growth: while pinned the
// transcript never detaches from the end, and a reader who scrolled up
// mid-stream is never yanked back down.
import { test, expect } from "./fixtures.js";

async function startLongProbe(page, session, command) {
  await page.goto(`/#session=${session.id}`);
  const prompt = page.getByLabel("Message or guidance");
  await expect(prompt).toBeVisible();
  await command("model", {
    session_id: session.id,
    generation: session.generation,
    operation: "select",
    model: "mock/model-b",
  });
  await prompt.fill("Long continuity probe");
  await prompt.press("Enter");
  await expect(page.locator(".composer .status-led.running")).toBeVisible();
  return prompt;
}

const gap = (page) =>
  page
    .locator(".transcript")
    .evaluate(
      (element) =>
        element.scrollHeight - element.scrollTop - element.clientHeight,
    );

test("streaming stays pinned to the end while following", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  await startLongProbe(page, session, command);
  // Sample the bottom gap across the whole stream. A sample can land
  // between a state apply and its pre-paint pin, so single transient
  // excursions are sampling race, not detachment: what matters is the
  // tail always recovers to the band and settles at zero when done.
  const gaps = [];
  for (let index = 0; index < 30; ++index) {
    gaps.push(await gap(page));
    await page.waitForTimeout(150);
  }
  expect(gaps.filter((value) => value > 100).length).toBeLessThanOrEqual(2);
  expect(gaps.slice(-5).every((value) => value <= 100)).toBe(true);
  await expect(page.locator(".composer .status-led.running")).toBeHidden({
    timeout: 60000,
  });
  await expect.poll(() => gap(page), { timeout: 15000 }).toBeLessThan(2);
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toHaveCount(0);
});

test("scrolled-up reader is never yanked down by streaming", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  await startLongProbe(page, session, command);
  // Wait for real overflow (total scrollable height), then move up out
  // of the stick band. Note: the bottom *gap* stays ~0 while pinned, so
  // overflow — not gap — is the precondition.
  await expect
    .poll(
      () =>
        page
          .locator(".transcript")
          .evaluate((element) => element.scrollHeight - element.clientHeight),
      { timeout: 30000 },
    )
    .toBeGreaterThan(400);
  const jump = page.getByRole("button", { name: "Jump to latest" });
  // Scroll up the way a reader does — repeated steps until the follow
  // releases. Relative steps self-heal like a human finger: if a stream
  // pin lands mid-gesture, the next step continues from wherever it
  // landed, and the loop only exits once unfollowed.
  for (let index = 0; index < 30; ++index) {
    if (await jump.isVisible()) break;
    await page.locator(".transcript").evaluate((element) => {
      element.scrollTop -= 200;
    });
    await page.waitForTimeout(50);
  }
  await expect(jump).toBeVisible();
  // Across the rest of the stream the reader must never move down.
  const tops = [];
  for (let index = 0; index < 20; ++index) {
    tops.push(
      await page
        .locator(".transcript")
        .evaluate((element) => element.scrollTop),
    );
    await page.waitForTimeout(150);
  }
  const settled = await page
    .locator(".transcript")
    .evaluate((element) => element.scrollTop);
  // Any sticky pin would jump thousands of px to the end; native
  // overflow-anchoring plus content-visibility re-estimates above the
  // viewport may legitimately drift the position within the band.
  expect(Math.max(...tops)).toBeLessThanOrEqual(settled + 100);
  await expect(page.locator(".composer .status-led.running")).toBeHidden({
    timeout: 60000,
  });
});

test("background completion badges while unfollowed, cleared on jump", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(180000);
  await startLongProbe(page, session, command);
  // Build scrollable history first, then let it finish while following.
  await expect(page.locator(".composer .status-led.running")).toBeHidden({
    timeout: 60000,
  });
  // A run tool needs approval: the ~10s sleep is the deterministic
  // window in which completion lands while unfollowed.
  const prompt = page.getByLabel("Message or guidance");
  await prompt.fill("Background activity probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.locator(".decision")).toContainText("BROWSER_ACTIVITY", {
    timeout: 30000,
  });
  await page.getByLabel("Response", { exact: true }).selectOption("y");
  await page
    .getByRole("button", { name: "Send response", exact: true })
    .click();
  await expect(page.locator(".tool-disclosure").first()).toBeVisible({
    timeout: 30000,
  });
  // Reader-like steps out of the stick band; the sleep is still running.
  for (let index = 0; index < 6; ++index) {
    await page.locator(".transcript").evaluate((element) => {
      element.scrollTop -= 60;
    });
    await page.waitForTimeout(100);
  }
  const jump = page.getByRole("button", { name: "Jump to latest" });
  await expect(jump).toBeVisible();
  // Completion lands blocks below while unfollowed: badge counts them.
  await expect(jump).toContainText(/\(\d+ new\)/, { timeout: 90000 });
  await jump.click();
  await expect(jump).toBeHidden();
});
