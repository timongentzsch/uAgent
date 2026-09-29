// A folder's coordinator opens from its header icon; its board lists the
// folder's sessions beside the chat, and above it on a phone.
import { test, expect } from "./fixtures.js";

const VIEWPORTS = [
  ["desktop", { width: 1440, height: 900 }],
  ["laptop", { width: 900, height: 860 }],
  ["phone", { width: 390, height: 844 }],
];

async function openCoordinator(page, session) {
  await page.goto(`/#session=${session.id}`);
  // Narrow layouts keep the sessions in a drawer.
  const drawer = page.getByLabel("Open sessions");
  const button = page.getByLabel("Open this folder's coordinator").first();
  await expect(drawer.or(button)).toBeVisible();
  if (await drawer.isVisible()) await drawer.click();
  await page.getByLabel("Open this folder's coordinator").first().click();
  await expect(
    page.getByRole("complementary", { name: "Board" }),
  ).toBeVisible();
  return new URL(page.url()).hash.match(/session=([a-f0-9]+)/)[1];
}

async function shot(page, name) {
  if (process.env.UAGENT_SCREENSHOTS) {
    await page.screenshot({
      path: `${process.env.UAGENT_SCREENSHOTS}/${name}.png`,
    });
  }
}

for (const [name, viewport] of VIEWPORTS) {
  test(`coordinator view and help (${name})`, async ({
    page,
    session,
    browserName,
  }) => {
    await page.setViewportSize(viewport);
    await openCoordinator(page, session);
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    // One stable geometry: nothing scrolls the page sideways.
    expect(
      await page.evaluate(
        () => document.documentElement.scrollWidth - window.innerWidth,
      ),
    ).toBeLessThanOrEqual(0);
    await page.getByLabel("What is the coordinator?").click();
    const help = page.locator(".coordinator-help");
    await expect(help.getByText("How it differs")).toBeVisible();
    // The help sizes to its text: no stretched rows or button.
    const button = help.getByRole("button", { name: "Edit soul" });
    expect((await button.boundingBox()).height).toBeLessThan(60);
    const panel = await help.boundingBox();
    const body = await help.locator(".coordinator-help-body").boundingBox();
    expect(panel.height - body.height).toBeLessThan(60);
    await shot(page, `help-${browserName}-${name}`);
  });

  test(`coordinator turns keep their order (${name})`, async ({
    page,
    session,
    command,
    request,
    browserName,
  }) => {
    await page.setViewportSize(viewport);
    const id = await openCoordinator(page, session);
    let meta;
    await expect
      .poll(async () => {
        meta = (await (await request.get(`/api/sessions/${id}`)).json())
          .metadata;
        return meta.generation;
      })
      .toBeTruthy();
    await command("model", {
      session_id: id,
      generation: meta.generation,
      operation: "select",
      model: "mock/model-b",
    });
    const prompt = page.getByLabel("Message or guidance");
    for (const [index, text] of ["first", "second", "third"].entries()) {
      await prompt.fill(text);
      await prompt.press("Enter");
      await expect(page.locator(".turn-summary")).toHaveCount(index + 1, {
        timeout: 15_000,
      });
    }
    // Each answer keeps its own stats line, in order, and what was typed is
    // shown as typed.
    const rows = await page
      .locator(".transcript-content > *")
      .evaluateAll((nodes) =>
        nodes.map((node) =>
          node.classList.contains("turn-summary")
            ? "stats"
            : node.classList.contains("user")
              ? node.textContent.replace(/^\s*\d+:\d+\s*(AM|PM)?/, "").trim()
              : "",
        ),
      );
    expect(rows.filter(Boolean)).toEqual([
      "first",
      "stats",
      "second",
      "stats",
      "third",
      "stats",
    ]);
    // At the bottom, there is nothing to jump to.
    await expect(page.locator(".jump")).toHaveCount(0);
    await shot(page, `turns-${browserName}-${name}`);
  });
}

for (const [name, viewport] of VIEWPORTS) {
  test(`decisions waiting on you show above the composer (${name})`, async ({
    page,
    session,
    command,
    browserName,
  }) => {
    await page.setViewportSize(viewport);
    // A conversation in the folder asks to save a memory, in Ask mode.
    await command("model", {
      session_id: session.id,
      generation: session.generation,
      operation: "select",
      model: "mock/model-b",
    });
    await command("submit", {
      session_id: session.id,
      generation: session.generation,
      text: "Memory receipt probe: please save this test memory",
    });
    await openCoordinator(page, session);
    const waiting = page.getByRole("region", {
      name: "Decisions waiting on you",
    });
    await expect(waiting.getByText("Needs your decision")).toBeVisible();
    await expect(waiting.getByText("Allow memory?")).toBeVisible();
    // The transcript keeps at least half the column.
    const box = await waiting.boundingBox();
    const column = await page.locator(".coordinator-chat").boundingBox();
    expect(box.height).toBeLessThanOrEqual(column.height / 2 + 1);
    await shot(page, `escalation-${browserName}-${name}`);
    await waiting.getByRole("button", { name: "Allow once" }).click();
    await expect(waiting).toHaveCount(0);
  });
}

test("the soul is edited in the system prompt editor", async ({
  page,
  session,
}) => {
  await openCoordinator(page, session);
  await page.getByLabel("What is the coordinator?").click();
  await page.getByRole("button", { name: "Edit soul" }).click();
  await expect(page.getByLabel("Prompt scope")).toHaveValue("soul-user");
  const text = page.getByLabel("Soul text");
  await expect(text).toBeEnabled();
  await text.fill("Prefer small threads.");
  await page.getByRole("button", { name: "Save" }).click();
  await expect(page.getByRole("button", { name: "Save" })).toBeDisabled();
  await page.getByLabel("Prompt scope").selectOption("global");
  await page.getByLabel("Prompt scope").selectOption("soul-user");
  await expect(page.getByLabel("Soul text")).toHaveValue(
    "Prefer small threads.",
  );
  await shot(page, "soul-in-prompt-editor");
});
