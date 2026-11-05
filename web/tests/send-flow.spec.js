// Sending: the message you sent is one row from the tap on, the transcript
// never shrinks before the answer starts, and "Working" is said once, by the
// status line under the input.
import { test, expect, online, selectProbeModel } from "./fixtures.js";

for (const [name, viewport] of [
  ["desktop", { width: 1280, height: 800 }],
  ["phone", { width: 390, height: 844 }],
])
  test(`a sent message keeps its row and says Working once (${name})`, async ({
    page,
    session,
    command,
  }) => {
    await page.setViewportSize(viewport);
    await selectProbeModel(command, session, { yolo: true });
    await page.goto(`/#session=${session.id}`);
    await online(page);
    // A conversation with a turn in it, so nothing gives way to the first.
    const input = page.getByLabel("Message or guidance");
    const idle = page.locator(".composer .status-led.running");
    await input.fill("Exploration probe");
    await page.locator(".composer-primary").click();
    await expect(idle).toBeHidden({ timeout: 60000 });
    await input.fill("Exploration probe");
    await page.evaluate(() => {
      const transcript = document.querySelector(".transcript");
      const seen = (window.sendFlow = { removed: 0, shrank: 0 });
      new MutationObserver((records) => {
        for (const record of records)
          for (const node of record.removedNodes)
            if (node instanceof Element && node.matches(".message.user"))
              seen.removed++;
      }).observe(transcript, { childList: true, subtree: true });
      // Until the answer starts, the rows only grow (the transcript's box
      // may shrink as the status line opens; what it holds may not).
      const extent = () =>
        [...transcript.querySelectorAll(".transcript-content > *")].reduce(
          (sum, row) => sum + row.getBoundingClientRect().height,
          0,
        );
      let height = extent();
      const sample = () => {
        if (transcript.querySelectorAll(".message.response").length > 1) return;
        // Under a pixel is text re-wrapping as the box resizes, not a move.
        if (extent() < height - 1) seen.shrank++;
        height = extent();
        requestAnimationFrame(sample);
      };
      requestAnimationFrame(sample);
    });
    await page.locator(".composer-primary").click();
    const status = page.locator(".status-line .working");
    await expect(status).toHaveText("Working");
    await expect(
      page.locator(".transcript").getByText("Working", { exact: true }),
    ).toHaveCount(0);
    await expect(idle).toBeHidden({ timeout: 60000 });
    expect(await page.evaluate(() => window.sendFlow)).toEqual({
      removed: 0,
      shrank: 0,
    });
  });
