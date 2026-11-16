// Every view's buttons on a phone: none cut or wrapped, no row of them wider
// than its box, nothing that scrolls the page sideways.
import { mkdir, realpath } from "node:fs/promises";
import {
  test,
  expect,
  headerAction,
  online,
  openFiles,
  openSheet,
  paper,
  saveHistory,
  savedConversation,
  selectProbeModel,
  sheet,
  showList,
  slide,
  writeFiles,
} from "./fixtures.js";

test.use({
  isMobile: true,
  hasTouch: true,
  viewport: { width: 390, height: 844 },
});

// What is cramped on the page now, each named so a failure says where.
const cramped = (page) =>
  page.evaluate(() => {
    const found = [];
    const shown = (node) => {
      const box = node.getBoundingClientRect();
      return (
        box.width > 0 &&
        box.height > 0 &&
        getComputedStyle(node).visibility !== "hidden" &&
        !node.closest("[inert], .xterm")
      );
    };
    const named = (node) =>
      (node.getAttribute("aria-label") || node.textContent || node.className)
        .trim()
        .replace(/\s+/g, " ")
        .slice(0, 40);
    // A box the user scrolls on purpose, sideways.
    const scrolls = (node) =>
      /auto|scroll/.test(getComputedStyle(node).overflowX);
    const scroller = (node) => {
      for (let at = node.parentElement; at; at = at.parentElement)
        if (scrolls(at) && at !== document.documentElement) return at;
      return null;
    };
    if (document.documentElement.scrollWidth > innerWidth + 1)
      found.push(
        `the page scrolls sideways by ${document.documentElement.scrollWidth - innerWidth}px`,
      );
    for (const button of document.querySelectorAll("button")) {
      if (!shown(button)) continue;
      const box = button.getBoundingClientRect();
      if (!scroller(button) && (box.left < -1 || box.right > innerWidth + 1))
        found.push(`off the screen: ${named(button)}`);
      // A label that wrapped: one text on two lines. A row may have a
      // line of its own for its details.
      const walk = document.createTreeWalker(button, NodeFilter.SHOW_TEXT);
      for (let text = walk.nextNode(); text; text = walk.nextNode()) {
        if (!text.textContent.trim()) continue;
        const range = document.createRange();
        range.selectNodeContents(text);
        const lines = new Set(
          [...range.getClientRects()]
            .filter((rect) => rect.width)
            .map((rect) => Math.round(rect.top / 4)),
        );
        if (lines.size > 1) {
          found.push(`wraps: ${named(button)}`);
          break;
        }
      }
      // Cut without the ellipsis that says so: a name that may be long is
      // cut on purpose.
      for (const part of [button, ...button.querySelectorAll("*")])
        if (
          shown(part) &&
          part.clientWidth > 1 &&
          part.scrollWidth > part.clientWidth + 1 &&
          getComputedStyle(part).textOverflow !== "ellipsis"
        ) {
          found.push(`cut: ${named(button)}`);
          break;
        }
    }
    // A row of buttons that its box does not hold.
    const rows = new Set();
    for (const button of document.querySelectorAll("button"))
      if (shown(button) && button.parentElement) rows.add(button.parentElement);
    for (const row of rows) {
      if (scrolls(row) || scroller(row)) continue;
      const buttons = [...row.children].filter(
        (child) => child.tagName === "BUTTON" && shown(child),
      );
      if (buttons.length < 2) continue;
      const box = row.getBoundingClientRect();
      if (
        buttons.some(
          (button) => button.getBoundingClientRect().right > box.right + 1,
        )
      )
        found.push(`a row wider than its box: ${named(row)}`);
    }
    for (const dialog of document.querySelectorAll(
      "dialog[open], .dialog-body",
    ))
      if (shown(dialog) && dialog.scrollWidth > dialog.clientWidth + 1)
        found.push(`a dialog scrolls sideways: ${named(dialog)}`);
    return [...new Set(found)];
  });

const roomy = async (page) => expect.poll(() => cramped(page)).toEqual([]);

// A page's tools in one row that needs no scrolling: those that are an
// icon alone fold into a menu.
const toolsFit = async (page) => {
  const tools = page.locator(".page-tools").first();
  expect(
    await tools.evaluate((row) => row.scrollWidth - row.clientWidth),
  ).toBeLessThanOrEqual(1);
  await tools.getByRole("button", { name: "More tools" }).click();
  await expect(
    page.getByRole("menuitem", { name: "Open alone", exact: true }),
  ).toBeVisible();
  await page.keyboard.press("Escape");
};

// A conversation saved in Scratch with the host's suggestion for it.
async function unfiled(host, request, title, suggestion) {
  const scratch = `${host.home}/.uagent/spaces/Scratch`;
  await mkdir(`${scratch}/.uagent`, { recursive: true });
  const id = crypto.randomUUID();
  await saveHistory(host, `saved-${id}.json`, {
    id,
    title,
    turns: 1,
    cwd: await realpath(scratch),
    suggestion,
    messages: [
      { role: "user", content: title },
      { role: "assistant", content: "Done" },
    ],
  });
  let found;
  await expect
    .poll(async () => {
      const response = await request.get("/api/sessions?refresh=1");
      const { sessions } = await response.json();
      found = sessions.find((item) => item.title === title);
      return !!found;
    })
    .toBe(true);
  return found.id;
}

test.describe("the pages of the list", () => {
  test.use({ places: true });

  test("the Library and its selection bar", async ({ page, host, session }) => {
    await writeFiles(host.project, {
      "design/landing/hero.html":
        '<!doctype html><meta name="artboard" content="390x844"><title>Hero</title>',
      "docs/memo.html":
        '<!doctype html><meta name="print" content="flow a4"><title>Memo</title>',
    });
    await page.goto(`/#session=${session.id}`);
    await showList(page);
    await page
      .locator("dialog.sidebar, aside.sidebar")
      .getByRole("button", { name: "Library", exact: true })
      .click();
    const rows = page.locator(".library-row:not(.placeholder *)");
    await expect(rows.first()).toBeVisible();
    await roomy(page);
    await rows.first().click({ modifiers: ["ControlOrMeta"] });
    await expect(
      page.getByRole("toolbar", { name: "Selection" }),
    ).toBeVisible();
    await roomy(page);
  });

  test("Scheduled and a new task", async ({ page }) => {
    await page.goto("/");
    await showList(page);
    await page
      .locator("dialog.sidebar, aside.sidebar")
      .getByRole("button", { name: "Scheduled", exact: true })
      .click();
    await page.getByRole("button", { name: "New task", exact: true }).click();
    await expect(page.getByLabel("Name", { exact: true })).toBeVisible();
    await roomy(page);
  });

  test("the conversations' selection bar", async ({ page, host, request }) => {
    for (const title of ["Alpha talk", "Bravo talk"])
      await savedConversation(host, request, title);
    await page.goto("/");
    await showList(page);
    await page
      .locator(".session-row", { hasText: "Alpha talk" })
      .getByRole("button", { name: "Conversation menu" })
      .press("Enter");
    await page.getByRole("menuitem", { name: "Select", exact: true }).click();
    await expect(
      page.getByRole("toolbar", { name: "Selection", exact: true }),
    ).toBeVisible();
    await roomy(page);
  });
});

test.describe("files", () => {
  test("a deck's tool row", async ({ page, session, host }) => {
    await writeFiles(host.project, {
      "deck/01-cover.html": slide("Cover", "Say hello."),
      "deck/02-plan.html": slide("Plan", "Three steps."),
    });
    await openFiles(page, session, "deck/", { phone: true });
    await expect(page.locator(".page-stage iframe").first()).toBeVisible();
    await roomy(page);
    await toolsFit(page);
  });

  test("a document's tool row", async ({ page, session, host }) => {
    await writeFiles(host.project, {
      "memo.html": paper("Memo", "<h1>Memo</h1><p>Text.</p>"),
    });
    await openFiles(page, session, "memo.html", { phone: true });
    await expect(page.locator(".page-stage iframe").first()).toBeVisible();
    await roomy(page);
    await toolsFit(page);
  });

  test("a sheet's Format sheet and rules dialog", async ({
    page,
    session,
    host,
  }) => {
    const { files, stage, cell } = await openSheet(
      page,
      session,
      host.project,
      sheet("Plan", { Tasks: ["<th>Task<th>Cost", "<td>Write<td>1200"] }),
      { phone: true },
    );
    await roomy(page);
    // Its tabs scroll, and keep room to show a tab whole.
    await cell("Tasks", "B2").click();
    expect(
      await files.locator(".sheet-tabs").evaluate((strip) => {
        const tab = strip.querySelector("[role=tab]").getBoundingClientRect();
        return tab.right <= strip.getBoundingClientRect().right + 1;
      }),
    ).toBe(true);
    await cell("Tasks", "A2").click();
    await files.getByRole("button", { name: "Format", exact: true }).click();
    await expect(page.getByRole("dialog", { name: "Format" })).toBeVisible();
    await roomy(page);
    await page.keyboard.press("Escape");
    await cell("Tasks", "B2").click({ button: "right" });
    await stage
      .getByRole("menuitem", { name: "Conditional formatting…" })
      .click();
    await expect(
      page.getByRole("dialog", { name: /^Conditional formatting/ }),
    ).toBeVisible();
    await roomy(page);
  });
});

test.describe("a conversation", () => {
  test.beforeEach(({ command, session }) => selectProbeModel(command, session));

  test("the terminal and its key bar", async ({ page, session }) => {
    await page.goto(`/#session=${session.id}`);
    await headerAction(page, "Terminal");
    await expect(page.locator(".terminal-keys")).toBeVisible();
    await roomy(page);
  });

  test("a receipt's Undo row", async ({ page, session }) => {
    await page.goto(`/#session=${session.id}`);
    await page.getByLabel("Message or guidance").fill("request approval");
    await page.getByRole("button", { name: "Send", exact: true }).click();
    await page.getByRole("button", { name: "Allow once", exact: true }).click();
    await page.getByRole("button", { name: /^Review changes: / }).click();
    await expect(
      page.getByRole("dialog", { name: "Changed files" }),
    ).toBeVisible();
    await roomy(page);
  });

  test("the providers form", async ({ page, session }) => {
    await page.goto(`/#session=${session.id}`);
    await headerAction(page, "Settings");
    await page
      .locator(".settings-nav")
      .getByRole("button", { name: "All conversations", exact: true })
      .click();
    await page.getByLabel("Find a setting").fill("Providers");
    await page.getByRole("button", { name: "Add a provider" }).click();
    const adding = page.getByRole("dialog", { name: "Add a provider" });
    await adding
      .getByRole("button", { name: "OpenRouter", exact: true })
      .click();
    await roomy(page);
  });
});

test.describe("a conversation in Scratch", () => {
  test.use({ places: true });

  test("the suggestion bar above the input", async ({
    page,
    host,
    request,
  }) => {
    const id = await unfiled(host, request, "Three days in Lisbon", {
      options: [{ workspace: "Lisbon trip" }],
      reason: "No place you have fits it.",
    });
    await page.goto(`/#session=${id}`);
    await online(page);
    await expect(
      page.getByRole("region", { name: "Suggested place" }),
    ).toBeVisible();
    await roomy(page);
  });
});
