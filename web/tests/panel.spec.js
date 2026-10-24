// A place's panel opens from the place's name; its board lists what it
// started beside the chat, and slides in from the right on a phone. What it
// started lists under the place's name in the sidebar.
import { mkdir, realpath, writeFile } from "node:fs/promises";
import {
  test,
  expect,
  openPanel,
  headerAction,
  selectProbeModel,
  openProbePanel,
} from "./fixtures.js";

const VIEWPORTS = [
  ["desktop", { width: 1440, height: 900 }],
  ["laptop", { width: 900, height: 860 }],
  ["phone", { width: 390, height: 844 }],
];

test("on a phone the board slides in from the right", async ({
  page,
  session,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await openPanel(page, session);
  // Nothing stacks above the chat.
  await expect(page.locator(".conversation .board")).toHaveCount(0);
  await page.getByRole("button", { name: /^Board/ }).click();
  const sheet = page.getByRole("dialog", { name: "Board" });
  await expect(
    sheet.getByRole("complementary", { name: "Board" }),
  ).toBeVisible();
  // Settled against the right edge, leaving the chat visible to its left.
  await expect
    .poll(async () => {
      const box = await sheet.boundingBox();
      return Math.round(box.x + box.width);
    })
    .toBe(390);
  expect((await sheet.boundingBox()).x).toBeGreaterThan(30);
  await page.keyboard.press("Escape");
  await expect(sheet).toHaveCount(0);
});

async function shot(page, name) {
  if (process.env.UAGENT_SCREENSHOTS) {
    await page.screenshot({
      path: `${process.env.UAGENT_SCREENSHOTS}/${name}.png`,
    });
  }
}

for (const [name, viewport] of VIEWPORTS) {
  test(`panel view and help (${name})`, async ({
    page,
    session,
    browserName,
  }) => {
    await page.setViewportSize(viewport);
    await openPanel(page, session);
    await expect(page.getByLabel("Message or guidance")).toBeVisible();
    // One stable geometry: nothing scrolls the page sideways.
    expect(
      await page.evaluate(
        () => document.documentElement.scrollWidth - window.innerWidth,
      ),
    ).toBeLessThanOrEqual(0);
    const head = page.locator(".conversation-head");
    if (name === "phone") {
      // A phone keeps the title, the board and the menu, which holds the
      // instructions.
      await expect(head.locator(".panel-subtitle")).toHaveCount(0);
      await head.getByRole("button", { name: "Conversation menu" }).click();
      await expect(
        page.getByRole("menuitem", { name: "Edit instructions" }),
      ).toBeVisible();
      await page.keyboard.press("Escape");
    } else {
      // What it is, in one line under its title, with its instructions a
      // tap away even where the words truncate.
      const subtitle = page.locator(".panel-subtitle");
      await expect(subtitle).toContainText("hands changes to threads");
      const button = subtitle.getByRole("button", {
        name: "Edit instructions",
      });
      await expect(button).toBeInViewport();
      expect((await subtitle.boundingBox()).height).toBeLessThan(40);
    }
    // Nothing the title column holds reaches under the buttons beside it.
    expect(
      await head.evaluate((node) => {
        const title = node.querySelector(":scope > div");
        const next = title.nextElementSibling;
        if (!next) return true;
        const edge = next.getBoundingClientRect().left;
        return [...title.querySelectorAll("*")].every(
          (child) => child.getBoundingClientRect().right <= edge + 0.5,
        );
      }),
    ).toBe(true);
    await shot(page, `help-${browserName}-${name}`);
  });

  test(`panel turns keep their order (${name})`, async ({
    page,
    session,
    command,
    request,
    browserName,
  }) => {
    await page.setViewportSize(viewport);
    await openProbePanel(page, session, command, request);
    const prompt = page.getByLabel("Message or guidance");
    for (const [index, text] of ["first", "second", "third"].entries()) {
      await prompt.fill(text);
      await prompt.press("Enter");
      await expect(page.locator(".turn-summary")).toHaveCount(index + 1);
    }
    // Each answer keeps its own stats line, in order, and what was typed is
    // shown as typed.
    const rows = await page
      .locator(".transcript-content > *")
      .evaluateAll((nodes) =>
        nodes.map((node) =>
          node.classList.contains("turn-footer")
            ? "stats"
            : node.classList.contains("user")
              ? // What shows: the row's spoken name ("You, 10:32") is not.
                [...node.children]
                  .filter((child) => !child.classList.contains("sr-only"))
                  .map((child) => child.textContent)
                  .join("")
                  .replace(/^\s*\d+:\d+\s*(AM|PM)?/, "")
                  .trim()
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
    await selectProbeModel(command, session);
    await command("submit", {
      session_id: session.id,
      generation: session.generation,
      text: "Memory receipt probe: please save this test memory",
    });
    await openPanel(page, session);
    // The panel is the place's name now, and what waits is said once in
    // the list: by the inbox above it.
    if (name === "desktop") {
      await expect(page.getByTitle(/^Panel of /)).toHaveAttribute(
        "aria-current",
        "page",
      );
    }
    const waiting = page.getByRole("region", {
      name: "Decisions waiting on you",
    });
    await expect(waiting.getByText("Needs your decision")).toBeVisible();
    await expect(waiting.getByText("Allow memory?")).toBeVisible();
    // The transcript keeps at least half the column.
    const box = await waiting.boundingBox();
    const column = await page.locator("#conversation").boundingBox();
    expect(box.height).toBeLessThanOrEqual(column.height / 2 + 1);
    await shot(page, `escalation-${browserName}-${name}`);
    await waiting.getByRole("button", { name: "Allow once" }).click();
    await expect(waiting).toHaveCount(0);
  });
}

test("the panel's subtitle opens its instructions", async ({
  page,
  session,
}) => {
  await openPanel(page, session);
  await headerAction(page, "Edit instructions");
  const dialog = page.getByRole("dialog", { name: "Instructions" });
  const panel = dialog.getByLabel("Yours · panel");
  await expect(panel).toBeEnabled();
  await panel.fill("Prefer small threads.");
  await dialog.getByRole("button", { name: "Save", exact: true }).click();
  await expect(dialog.getByRole("button", { name: "Save" })).toHaveCount(0);
  // Saving keeps focus in the field, not on the page behind the dialog.
  await expect(panel).toBeFocused();
  await shot(page, "instructions");
});

test("instructions read well on a phone", async ({ page, session }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await openPanel(page, session);
  await headerAction(page, "Edit instructions");
  const dialog = page.getByRole("dialog", { name: "Instructions" });
  await expect(dialog.getByLabel("Yours · panel")).toBeEnabled();
  expect(
    await dialog.evaluate((node) => node.scrollWidth <= node.clientWidth + 1),
  ).toBe(true);
  await shot(page, "instructions-phone");
});

// The native session and workspace hash (src/core/strings.cc HashHex): FNV-1a
// over 64 bits.
function fnv1a64(text) {
  let value = 14695981039346656037n;
  for (const byte of new TextEncoder().encode(text)) {
    value = BigInt.asUintN(64, (value ^ BigInt(byte)) * 0x100000001b3n);
  }
  return value.toString(16).padStart(16, "0");
}

// A session file in the current shape, beside the panel's.
async function seed(dir, name, header) {
  const state = {
    messages: [{ role: "user", content: "go" }],
    message_kinds: ["user"],
    archive: [],
    archive_dropped_segments: 0,
    context_tokens: 0,
    context_window: 0,
    usage: {},
    last_sent_prompt: "",
    display: {},
    delivered_mail: [],
  };
  await writeFile(
    `${dir}/${name}.json`,
    `${JSON.stringify({ format: 3, model: "test", turns: 1, session_id: name, ...header })}\n${JSON.stringify(state)}`,
    { mode: 0o600 },
  );
}

test("the board shows the panel's plan with what works for each step", async ({
  page,
  host,
  session,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  const project = await realpath(host.project);
  const dir = `${host.home}/.uagent/history/${fnv1a64(project)}`;
  const panel = `${dir}/panel.json`;
  await mkdir(dir, { recursive: true });
  await writeFile(
    `${panel}.pinned`,
    JSON.stringify({
      goals: "Review the proxy",
      steps: [
        {
          id: "s1",
          title: "Survey the code",
          status: "done",
          result: "3 issues held",
        },
        { id: "s2", title: "Fix the lexer", status: "running" },
      ],
    }),
  );
  const role = { parent: fnv1a64(panel), parent_session: panel };
  await seed(dir, "thread-lexer", {
    cwd: project,
    title: "Lexer fix",
    kind: "thread",
    delegation: { ...role, lifetime: "thread", folder: project, step: "s2" },
  });
  await seed(dir, "agent-aaaa1111", {
    cwd: project,
    title: "review",
    tokens: 12400,
    delegation: { ...role, name: "reviewer-a", label: "Review a", step: "s1" },
  });
  await openPanel(page, session);
  const board = page.getByRole("complementary", { name: "Board" });
  await expect(board.getByRole("heading", { name: /^Plan/ })).toContainText(
    "1/2",
  );
  await expect(board).toContainText("Review the proxy");
  // A running step is open with its thread; it is not listed again as work.
  const running = board
    .locator(".plan-step")
    .filter({ hasText: "Fix the lexer" });
  await expect(running.getByText("Lexer fix")).toBeVisible();
  await expect(
    board
      .locator("section")
      .filter({ hasText: /^Work/ })
      .getByText("Lexer fix"),
  ).toHaveCount(0);
  // A finished one opens to its result and its subagents, one opening the
  // inspector.
  const done = board
    .locator(".plan-step")
    .filter({ hasText: "Survey the code" });
  await expect(done).toContainText("1 subagent");
  await done.locator("summary").click();
  await expect(done.getByText("3 issues held")).toBeVisible();
  await expect(done).toContainText("12.4k tokens");
  await done.getByRole("button", { name: /reviewer-a/ }).click();
  await expect(
    page.getByRole("dialog").filter({ hasText: "reviewer-a" }),
  ).toBeVisible();
});
