// An answer given while the turn's agent works is held as a step of the work
// until the agent reports: the person sees one answer per question, the
// status line names whom the turn waits for, and nothing above jumps.
import { mkdir } from "node:fs/promises";
import {
  test,
  expect,
  online,
  selectProbeModel,
  writeFiles,
} from "./fixtures.js";

const shots = process.env.UAGENT_SHOTS;
const shoot = async (page, name) => {
  if (!shots) return;
  await mkdir(shots, { recursive: true });
  await page.screenshot({ path: `${shots}/${name}.png` });
};

// The model's replies at turn level: the turn's answer, not a step of its
// work (an agent's report is drawn as a reply too, under its name).
const answers = (page) =>
  page.evaluate(() =>
    [...document.querySelectorAll(".transcript .message.response")]
      .filter(
        (reply) =>
          !reply.closest('[data-kind="work"]') &&
          reply.querySelector("h3")?.textContent.startsWith("Assistant"),
      )
      .map((reply) => reply.querySelector(".markdown")?.textContent.trim()),
  );

async function start(page, command, session, viewport, text) {
  await page.setViewportSize(viewport);
  await selectProbeModel(command, session, { yolo: true });
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.getByLabel("Message or guidance").fill(text);
  // Every frame from the send on: the top of the person's message, and
  // whether the draft that will be held is drawn as the turn's answer.
  await page.evaluate(() => {
    const seen = (window.userTops = []);
    window.draftAsAnswer = 0;
    const sample = () => {
      const row = document.querySelector(".transcript .message.user");
      if (row) seen.push(row.getBoundingClientRect().top);
      for (const reply of document.querySelectorAll(
        ".transcript .message.response",
      ))
        if (
          !reply.closest('[data-kind="work"]') &&
          reply.textContent.includes("Held draft answer")
        )
          window.draftAsAnswer++;
      if (!window.stopSampling) requestAnimationFrame(sample);
    };
    requestAnimationFrame(sample);
  });
  await page.locator(".composer-primary").click();
}

const work = (page) => page.locator('.transcript .node[data-kind="work"]');
// Opens the work, which shows its steps open only while it runs.
async function openWork(page) {
  const row = work(page).locator("> .node-row");
  if ((await row.getAttribute("aria-expanded")) !== "true") await row.click();
}
// The turn has ended: the button sends again (its agent may still work).
const idle = (page) =>
  expect(page.locator(".composer-primary")).toHaveAttribute(
    "data-mode",
    "send",
    { timeout: 30000 },
  );

for (const [name, viewport] of [
  ["desktop", { width: 1280, height: 800 }],
  ["phone", { width: 390, height: 844 }],
])
  test(`a held answer waits for the agent's report (${name})`, async ({
    page,
    session,
    command,
    host,
  }) => {
    await start(page, command, session, viewport, "Hold probe");
    // Held: a step of the running work, which names whom it waits for, as
    // the status line does; the turn has no answer yet.
    await expect(work(page)).toContainText("Waiting for Hold brief");
    await expect(work(page)).toHaveClass(/\brunning\b/);
    await expect(page.locator(".status-line")).toContainText(
      "Waiting for Hold brief",
    );
    await expect(work(page).locator(".message.response")).toContainText(
      "Held draft answer",
    );
    expect(await answers(page)).toEqual([]);
    const waiting = work(page).locator('.node[data-kind="notice"]');
    await expect(waiting).toHaveText(/Waiting for Hold brief/);
    await expect(waiting).toHaveClass(/\brunning\b/);
    await shoot(page, `held-${name}`);

    await writeFiles(host.project, { "hold-release": "" });
    await idle(page);
    // One answer, the one written with the report; the wait is done.
    await expect
      .poll(() => answers(page))
      .toEqual(["Final answer with the report"]);
    await shoot(page, `answered-${name}`);
    await openWork(page);
    await expect(waiting).toHaveText(/Waited for Hold brief/);
    await expect(waiting).toHaveClass(/\bok\b/);
    await shoot(page, `answered-open-${name}`);
    // The person's message never moved while the answer was held or moved
    // into the work.
    const tops = await page.evaluate(() => {
      window.stopSampling = true;
      return window.userTops;
    });
    // The held draft was never drawn as the turn's answer, not even while
    // it streamed.
    expect(await page.evaluate(() => window.draftAsAnswer)).toBe(0);
    expect(tops.length).toBeGreaterThan(0);
    expect(Math.max(...tops) - Math.min(...tops)).toBeLessThan(1);
  });

test.describe("when the agent never reports in time", () => {
  test.use({ agentsWait: 1 });
  for (const [name, viewport] of [
    ["desktop", { width: 1280, height: 800 }],
    ["phone", { width: 390, height: 844 }],
  ])
    test(`the answer stands with a notice after three holds (${name})`, async ({
      page,
      session,
      command,
      host,
    }) => {
      await start(page, command, session, viewport, "Cap probe");
      await idle(page);
      await expect.poll(() => answers(page)).toEqual(["Held draft answer"]);
      await openWork(page);
      const notices = work(page).locator('.node[data-kind="notice"]');
      await expect(notices).toHaveText([
        /Waited for Stuck brief/,
        /Waited for Stuck brief/,
        /Waited for Stuck brief/,
        /Answered before Stuck brief reported/,
      ]);
      // A notice in ordinary colours, as the agent's start row is drawn.
      const notice = notices.last();
      await expect(notice).not.toHaveClass(/\bfailed\b/);
      const color = (row) =>
        row
          .locator("> .node-row .node-headline")
          .evaluate((element) => getComputedStyle(element).color);
      expect(await color(notice)).toBe(
        await color(work(page).locator('.node[data-tool="agent_start"]')),
      );
      await shoot(page, `stood-${name}`);
      await writeFiles(host.project, { "stuck-release": "" });
    });
});
