// While a conversation is followed, what arrives glides in: the view eases to
// the end, a new row fades in (never on load), a sent message rises.
import { test, expect, online, selectProbeModel } from "./fixtures.js";

// Rows that fade in, and the sent message's rise, from the next send on.
const watch = (page) =>
  page.evaluate(() => {
    window.arrived = { faded: 0, rose: 0 };
    document.addEventListener("transitionrun", (event) => {
      if (
        event.propertyName === "opacity" &&
        event.target.closest?.(".transcript-content")
      )
        window.arrived.faded++;
    });
    // The rise is played by script: note each animation started on it.
    const animate = Element.prototype.animate;
    Element.prototype.animate = function (keyframes, options) {
      if (
        this.dataset?.messageId?.startsWith("outgoing-") &&
        keyframes?.[0]?.translate
      )
        window.arrived.rose = 1;
      return animate.call(this, keyframes, options);
    };
  });

// Sends and waits for the turn to finish: its footer is drawn.
async function send(page, text) {
  const footers = page.locator(".transcript .turn-footer");
  const before = await footers.count();
  await page.getByLabel("Message or guidance").fill(text);
  await page.locator(".composer-primary").click();
  await expect(footers).toHaveCount(before + 1, { timeout: 60000 });
}

// The gap under the view once nothing changes for a while.
const restingGap = (page) =>
  page.evaluate(
    () =>
      new Promise((done) => {
        const box = document.querySelector(".transcript");
        let last = -1;
        let still = 0;
        const tick = () => {
          const gap = box.scrollHeight - box.clientHeight - box.scrollTop;
          still = box.scrollHeight === last ? still + 1 : 0;
          last = box.scrollHeight;
          if (still > 30) done(gap);
          else requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
      }),
  );

test("what arrives glides in, and nothing animates on load", async ({
  page,
  session,
  command,
}) => {
  // The suite runs with motion off; this one is about the motion.
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await selectProbeModel(command, session, { yolo: true });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await send(page, "Exploration probe");
  await page.reload();
  await online(page);
  await watch(page);
  await page.waitForTimeout(300);
  expect(await page.evaluate(() => window.arrived)).toEqual({
    faded: 0,
    rose: 0,
  });
  await send(page, "Structured stream probe");
  const arrived = await page.evaluate(() => window.arrived);
  expect(arrived.rose).toBe(1);
  expect(arrived.faded).toBeGreaterThan(0);
  expect(await restingGap(page)).toBeLessThan(1);
});

test("with motion off nothing glides, fades or rises", async ({
  page,
  session,
  command,
}) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await selectProbeModel(command, session, { yolo: true });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await watch(page);
  await send(page, "Structured stream probe");
  expect(await page.evaluate(() => window.arrived)).toEqual({
    faded: 0,
    rose: 0,
  });
  expect(await restingGap(page)).toBeLessThan(1);
});
