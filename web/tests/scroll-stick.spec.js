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

test("progressive rendering retains text while syntax highlighting loads", async ({
  page,
  session,
  command,
}) => {
  let release;
  const loading = new Promise((resolve) => (release = resolve));
  await page.route("**/assets/highlight-*.js", async (route) => {
    await loading;
    await route.continue();
  });
  try {
    await startLongProbe(page, session, command);
    const stream = page.locator(".message.response .markdown[data-streaming]");
    await expect
      .poll(async () => (await stream.textContent())?.length || 0)
      .toBeGreaterThan(2000);
    // Content stays visible while highlighting is still loading.
    await expect(stream).toContainText("print('stable copy control')");
    // Code streams inside its block, not as raw fenced text, and offers no
    // copy until its fence closes.
    await expect(stream.locator(".code-block").first()).toBeVisible();
    await expect(stream).not.toContainText("```");
    release();
    await expect(page.locator(".composer .status-led.running")).toBeHidden();
    const final = page.locator(".message.response > .markdown");
    await expect(final).toContainText("print('stable copy control')");
    await expect(final).toContainText(
      "unfinished-looking content retained safely",
    );
  } finally {
    release();
  }
});

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
  await expect.poll(() => gap(page)).toBeLessThan(2);
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
  await expect.poll(() => gap(page)).toBeGreaterThan(100);
  // Across the rest of the stream the reader stays away from the live edge.
  const gaps = [];
  for (let index = 0; index < 20; ++index) {
    gaps.push(
      await page
        .locator(".transcript")
        .evaluate(
          (element) =>
            element.scrollHeight - element.scrollTop - element.clientHeight,
        ),
    );
    await page.waitForTimeout(150);
  }
  // A row can briefly shrink during rich-content replacement and clamp the
  // scroll range; the saved anchor must bring the reader back once it grows.
  expect(gaps.filter((value) => value <= 100).length).toBeLessThanOrEqual(2);
  expect(gaps.at(-1)).toBeGreaterThan(100);
  await expect(jump).toBeVisible();
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
  await page.getByRole("button", { name: "Allow once", exact: true }).click();
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

test("wheeling back down to the end follows again mid-stream", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  await startLongProbe(page, session, command);
  const transcript = page.locator(".transcript");
  await expect
    .poll(
      () =>
        transcript.evaluate(
          (element) => element.scrollHeight - element.clientHeight,
        ),
      { timeout: 30000 },
    )
    .toBeGreaterThan(400);
  const jump = page.getByRole("button", { name: "Jump to latest" });
  await transcript.hover();
  for (let index = 0; index < 30 && !(await jump.isVisible()); ++index) {
    await page.mouse.wheel(0, -200);
    await page.waitForTimeout(50);
  }
  await expect(jump).toBeVisible();
  // Real wheel steps, while content keeps growing and compensation runs:
  // reaching the end must clear the button without a click.
  for (let index = 0; index < 60 && (await jump.isVisible()); ++index) {
    await page.mouse.wheel(0, 400);
    await page.waitForTimeout(60);
  }
  await expect(jump).toBeHidden();
  await expect.poll(() => gap(page)).toBeLessThan(100);
  await expect(page.locator(".composer .status-led.running")).toBeHidden({
    timeout: 60000,
  });
});

test("a wheel at the end with no room below follows again", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  await startLongProbe(page, session, command);
  await expect(page.locator(".composer .status-led.running")).toBeHidden({
    timeout: 60000,
  });
  const transcript = page.locator(".transcript");
  const jump = page.getByRole("button", { name: "Jump to latest" });
  // Unfollowed while sitting at the end: the state a shrink that clamps the
  // range leaves behind. No scroll event can follow from here.
  await transcript.hover();
  await page.mouse.wheel(0, -300);
  await expect(jump).toBeVisible();
  // Content below the reader shrinks away; the browser clamps the range.
  await page
    .locator(".message")
    .last()
    .evaluate((message) => {
      message.style.display = "none";
    });
  await expect.poll(() => gap(page)).toBeLessThan(2);
  await expect(jump).toBeVisible();
  await page.mouse.wheel(0, 100);
  await expect(jump).toBeHidden();
});

test("a wheel up inside nested output leaves following alone", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  await startLongProbe(page, session, command);
  await expect(page.locator(".composer .status-led.running")).toBeHidden({
    timeout: 60000,
  });
  await expect.poll(() => gap(page)).toBeLessThan(2);
  // A tool-output-like scroller scrolled down inside the last message.
  await page
    .locator(".message")
    .last()
    .evaluate((message) => {
      const inner = document.createElement("pre");
      inner.className = "nested-probe";
      inner.style.cssText = "height: 60px; overflow: auto; margin: 0";
      inner.textContent = "line\n".repeat(50);
      message.append(inner);
      inner.scrollTop = inner.scrollHeight;
    });
  await expect.poll(() => gap(page)).toBeLessThan(2);
  const inner = page.locator(".nested-probe");
  const before = await inner.evaluate((element) => element.scrollTop);
  await inner.hover();
  await page.mouse.wheel(0, -40);
  // The wheel reached the nested scroller, so its handler has run.
  await expect
    .poll(() => inner.evaluate((element) => element.scrollTop))
    .toBeLessThan(before);
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toHaveCount(0);
});
