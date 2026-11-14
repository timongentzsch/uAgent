// Follow-tail under live streaming: while pinned the transcript stays at the
// end, and a reader who scrolled up mid-stream is never pulled back down.
import { test, expect, online, selectProbeModel } from "./fixtures.js";

async function startProbe(
  page,
  session,
  command,
  probe = "Long continuity probe",
) {
  await page.goto(`/#session=${session.id}`);
  const prompt = page.getByLabel("Message or guidance");
  await online(page);
  await selectProbeModel(command, session);
  await prompt.fill(probe);
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

// One sample per frame: samples by the clock thin out on a slow machine.
async function framesUntilIdle(page) {
  await page.locator(".transcript").evaluate((element) => {
    const samples = (window.samples = []);
    const frame = () => {
      samples.push({
        gap: element.scrollHeight - element.scrollTop - element.clientHeight,
        height: element.scrollHeight,
      });
      window.gapFrame = requestAnimationFrame(frame);
    };
    frame();
  });
  await expect(page.locator(".composer .status-led.running")).toBeHidden({
    timeout: 60000,
  });
  return page.evaluate(() => {
    cancelAnimationFrame(window.gapFrame);
    return window.samples;
  });
}
const gapsUntilIdle = async (page) =>
  (await framesUntilIdle(page)).map((frame) => frame.gap);

test("progressive rendering retains text while syntax highlighting loads", async ({
  page,
  session,
  command,
}) => {
  const { promise: loading, resolve: release } = Promise.withResolvers();
  await page.route("**/assets/highlight-*.js", async (route) => {
    await loading;
    await route.continue();
  });
  try {
    await startProbe(page, session, command);
    const stream = page.locator(".message.response .markdown[data-streaming]");
    await expect
      .poll(async () => (await stream.textContent())?.length || 0)
      .toBeGreaterThan(2000);
    await expect(stream).toContainText("print('stable copy control')");
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

// A line still being written may not yet decide its block; shown early and
// then undone, everything above the end would drop for a frame.
test("streamed content only grows and stays pinned to the end", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  await startProbe(page, session, command, "Structured stream probe");
  const reply = page.locator(".message.response").last();
  const textTop = () =>
    reply.evaluate(
      (element) =>
        element.querySelector(".markdown").getBoundingClientRect().top -
        element.getBoundingClientRect().top,
    );
  await expect(reply.locator(".markdown[data-streaming]")).toBeVisible();
  const streaming = await textTop();
  const frames = await framesUntilIdle(page);
  const shrinks = frames.filter(
    (frame, index) => index && frame.height < frames[index - 1].height,
  );
  expect(shrinks).toEqual([]);
  // The reply outgrows the view, so there is an end to stay pinned to.
  expect(frames.at(-1).height - frames[0].height).toBeGreaterThan(400);
  // A frame can land between a state apply and its pre-paint pin, so a single
  // excursion is not detachment.
  const gaps = frames.map((frame) => frame.gap);
  expect(gaps.filter((value) => value > 100).length).toBeLessThanOrEqual(2);
  expect(gaps.slice(-5).every((value) => value <= 100)).toBe(true);
  expect(gaps.at(-1)).toBeLessThan(2);
  expect(await textTop()).toBe(streaming);
  await expect(
    page.getByRole("button", { name: "Jump to latest" }),
  ).toHaveCount(0);
});

// WebKit sets the scroller back to an older offset as streamed text lands;
// the glide must not follow it back.
test("a glide that follows a long reply never steps back", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  await page.emulateMedia({ reducedMotion: "no-preference" });
  const prompt = await startProbe(page, session, command);
  await expect(page.locator(".composer .status-led.running")).toBeHidden({
    timeout: 60000,
  });
  await prompt.fill("Long continuity probe");
  await prompt.press("Enter");
  await expect(
    page.locator(".message.response .markdown[data-streaming]"),
  ).toBeVisible();
  await page.locator(".transcript").evaluate((element) => {
    window.tops = [];
    const frame = () => {
      window.tops.push(element.scrollTop);
      window.topFrame = requestAnimationFrame(frame);
    };
    frame();
  });
  await expect(page.locator(".composer .status-led.running")).toBeHidden({
    timeout: 60000,
  });
  const tops = await page.evaluate(() => {
    cancelAnimationFrame(window.topFrame);
    return window.tops;
  });
  const steps = tops.filter((top, index) => index && top < tops[index - 1] - 1);
  expect(steps).toEqual([]);
});

test("scrolled-up reader is never yanked down by streaming", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(120000);
  await startProbe(page, session, command);
  // Overflow is the precondition: the bottom gap stays ~0 while pinned.
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
  // Relative steps continue from wherever a stream pin lands mid-gesture.
  for (let index = 0; index < 30; ++index) {
    if (await jump.isVisible()) break;
    await page.locator(".transcript").evaluate((element) => {
      element.scrollTop -= 200;
    });
    await page.waitForTimeout(50);
  }
  await expect(jump).toBeVisible();
  await expect.poll(() => gap(page)).toBeGreaterThan(100);
  const gaps = await gapsUntilIdle(page);
  // A row can briefly shrink during rich-content replacement and clamp the
  // scroll range; the saved anchor must bring the reader back once it grows.
  expect(gaps.filter((value) => value <= 100).length).toBeLessThanOrEqual(2);
  expect(gaps.at(-1)).toBeGreaterThan(100);
  await expect(jump).toBeVisible();
});

test("background completion badges while unfollowed, cleared on jump", async ({
  page,
  session,
  command,
}) => {
  test.setTimeout(180000);
  await startProbe(page, session, command);
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
  await expect(page.locator(".node-row").first()).toBeVisible({
    timeout: 30000,
  });
  for (let index = 0; index < 6; ++index) {
    await page.locator(".transcript").evaluate((element) => {
      element.scrollTop -= 60;
    });
    await page.waitForTimeout(100);
  }
  const jump = page.getByRole("button", { name: "Jump to latest" });
  await expect(jump).toBeVisible();
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
  await startProbe(page, session, command);
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
  // Real wheel steps: reaching the end clears the button without a click.
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
  await startProbe(page, session, command);
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
  await startProbe(page, session, command);
  await expect(page.locator(".composer .status-led.running")).toBeHidden({
    timeout: 60000,
  });
  await expect.poll(() => gap(page)).toBeLessThan(2);
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
