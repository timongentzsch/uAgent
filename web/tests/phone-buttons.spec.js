// Every view's buttons on a phone: none cut or wrapped, no row of them wider
// than its box, nothing that scrolls the page sideways, and each of them
// shows a press.
import {
  test,
  expect,
  headerAction,
  online,
  openFiles,
  openSheet,
  paper,
  SAMPLE_ASK,
  savedConversation,
  selectProbeModel,
  servePending,
  sheet,
  showList,
  slide,
  unfiled,
  writeFiles,
} from "./fixtures.js";
import { cramped, roomy } from "./roomy.js";

test.use({
  isMobile: true,
  hasTouch: true,
  viewport: { width: 390, height: 844 },
});

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
    const name = page.getByLabel("Name", { exact: true });
    await expect(name).toBeVisible();
    // The way back keeps its row over the first field.
    const back = await page
      .getByRole("button", { name: "All runs", exact: true })
      .boundingBox();
    expect(back.height).toBeGreaterThanOrEqual(44);
    expect(back.y + back.height).toBeLessThanOrEqual(
      (await page.locator(".field", { has: name }).boundingBox()).y,
    );
    await roomy(page);
  });

  test("the home start box with a file", async ({ page }) => {
    await page.goto("/");
    const box = page.locator(".start-field");
    await expect(box.getByLabel("Start a conversation")).toBeEnabled();
    await box.getByLabel("Attach files").setInputFiles({
      name: "a-long-name-for-a-file-from-the-phone.txt",
      mimeType: "text/plain",
      buffer: Buffer.from("aside"),
    });
    await expect(box.locator(".file-chip")).toHaveCount(1);
    await roomy(page);
  });

  test("the conversations' selection bar", async ({ page, host, request }) => {
    for (const title of ["Alpha talk", "Bravo talk"])
      await savedConversation(host, request, title);
    await page.goto("/");
    await showList(page);
    // The drawer shows its title whole, and leaves an edge of the page.
    const drawer = page.locator("dialog.sidebar");
    expect(
      await drawer
        .locator("header h2")
        .evaluate((title) => title.scrollWidth - title.clientWidth),
    ).toBeLessThanOrEqual(0);
    expect((await drawer.boundingBox()).width).toBe(390 - 16);
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
    await expect(
      page.getByRole("button", { name: "Undo", exact: true }),
    ).toHaveCount(0);
    await page.getByRole("button", { name: "More tools" }).click();
    await expect(
      page.getByRole("menuitem", { name: "Undo", exact: true }),
    ).toBeDisabled();
    await expect(
      page.getByRole("menuitem", { name: "Redo", exact: true }),
    ).toBeDisabled();
    await page.keyboard.press("Escape");
    await toolsFit(page);
  });

  test("a document's tool row, and Edit's bars over a selection and at the foot", async ({
    page,
    session,
    host,
  }) => {
    await writeFiles(host.project, {
      "memo.html": paper("Memo", "<h1>Memo</h1><p>Text.</p>"),
    });
    const files = await openFiles(page, session, "memo.html", {
      phone: true,
    });
    await expect(page.locator(".page-stage iframe").first()).toBeVisible();
    await roomy(page);
    await toolsFit(page);
    await files.getByRole("button", { name: "Edit" }).click();
    await expect(files.getByRole("group", { name: "Insert" })).toBeVisible();
    const stage = page.frameLocator(".page-stage iframe:not([inert])");
    const text = stage.getByText("Text.");
    const box = await text.boundingBox();
    await page.touchscreen.tap(box.x + box.width / 2, box.y + box.height / 2);
    await expect(text).toBeFocused();
    await text.evaluate((block) => getSelection().selectAllChildren(block));
    const bar = page.getByRole("toolbar", { name: "Text" });
    await expect(bar).toBeVisible();
    // Under the selection: the phone's own menu stands over it.
    await expect
      .poll(async () => {
        const [over, under] = await Promise.all([
          bar.boundingBox(),
          text.boundingBox(),
        ]);
        return !!over && over.y >= under.y + under.height - 1;
      })
      .toBe(true);
    await roomy(page);
  });

  test("a whiteboard with Edit off and on, and the bar over its picked shape", async ({
    page,
    session,
    host,
  }) => {
    await writeFiles(host.project, {
      "boards/flow/board.html": `<!doctype html>
<meta charset="utf-8">
<meta name="artboard" content="1000x600">
<meta name="board">
<title>Flow</title>
<div id="ok" data-shape="parallelogram" data-fill="orange" data-at="400 300" data-size="160 100">Valid?</div>
`,
    });
    const files = await openFiles(page, session, "boards/flow/", {
      phone: true,
    });
    const stage = page.frameLocator(".page-stage iframe:not([inert])");
    await expect(stage.locator("#ok")).toBeVisible();
    await roomy(page);
    await files.getByRole("button", { name: "Edit" }).click();
    await expect(files.getByRole("group", { name: "Insert" })).toBeVisible();
    await roomy(page);
    const box = await stage.locator("#ok").boundingBox();
    await page.touchscreen.tap(box.x + box.width / 2, box.y + box.height / 2);
    await expect(page.getByRole("toolbar", { name: "Picked" })).toBeVisible();
    await roomy(page);
    await toolsFit(page);
  });

  test("a slide's tools and the bar over its picked element", async ({
    page,
    session,
    host,
  }) => {
    await writeFiles(host.project, {
      "deck/01-cover.html": slide(
        "Cover",
        "Say hello.",
        '<div class="card"><p style="translate: 5px 0px;">Inside.</p></div><div data-shape="rect" style="position: absolute; left: 100px; top: 300px; width: 200px; height: 100px;"></div>',
      ),
      "deck/02-plan.html": slide("Plan", "Three steps."),
    });
    const files = await openFiles(page, session, "deck/", { phone: true });
    const stage = page.frameLocator(".page-stage iframe:not([inert])");
    await expect(stage.locator("h1")).toBeVisible();
    await files.getByRole("button", { name: "Edit" }).click();
    await expect(files.getByRole("group", { name: "Insert" })).toBeVisible();
    await roomy(page);
    // Every control the bar has: a container and Put back, a shape's order.
    for (const element of [
      stage.getByText("Inside."),
      stage.locator("[data-shape]"),
    ]) {
      const box = await element.boundingBox();
      await page.touchscreen.tap(box.x + box.width / 2, box.y + box.height / 2);
      await expect(page.getByRole("toolbar", { name: "Picked" })).toBeVisible();
      await roomy(page);
    }
    await toolsFit(page);
  });

  test("a sheet's format bar, its tools and its rules dialog", async ({
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
    // The format bar keeps inside the stage and scrolls sideways.
    const format = files.getByRole("toolbar", { name: "Format" });
    await expect(format).toBeVisible();
    const [room, bar] = await Promise.all([
      files.locator(".page-stage").boundingBox(),
      format.boundingBox(),
    ]);
    expect(bar.x).toBeGreaterThanOrEqual(room.x);
    expect(bar.x + bar.width).toBeLessThanOrEqual(room.x + room.width);
    await roomy(page);
    await toolsFit(page);
    // Its tabs scroll, and keep room to show a tab whole.
    await cell("Tasks", "B2").click();
    expect(
      await files.locator(".sheet-tabs").evaluate((strip) => {
        const tab = strip.querySelector("[role=tab]").getBoundingClientRect();
        return tab.right <= strip.getBoundingClientRect().right + 1;
      }),
    ).toBe(true);
    expect(
      (await files.getByRole("tab", { name: "Tasks" }).boundingBox()).height,
    ).toBeGreaterThanOrEqual(44);
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

  test("an answer's link, file reference, code and source", async ({
    page,
    host,
    request,
  }) => {
    const { id } = await savedConversation(
      host,
      request,
      "Rich answer",
      "See [the docs](https://example.com) and `web/src/app.tsx:12`.\n\n```js\nlet a = 1;\n```\n\n```mermaid\ngraph TD; A-->B;\n```\n",
    );
    await page.goto(`/#session=${id}`);
    const transcript = page.locator(".transcript");
    await expect(transcript.locator(".file-ref")).toBeVisible();
    await expect(transcript.locator("summary")).toBeVisible();
    // Drawn: its controls are no longer replaced under the probe.
    await expect(
      transcript.getByRole("img", { name: "Mermaid diagram" }),
    ).toBeVisible();
    await roomy(page);
  });

  test("a sent picture and file", async ({ page, session }) => {
    await page.goto(`/#session=${session.id}`);
    await online(page);
    await page.locator('.composer input[type="file"]').setInputFiles([
      {
        name: "shot.svg",
        mimeType: "image/svg+xml",
        buffer: Buffer.from(
          '<svg xmlns="http://www.w3.org/2000/svg" width="4" height="3"/>',
        ),
      },
      { name: "notes.txt", mimeType: "text/plain", buffer: Buffer.from("hi") },
    ]);
    await expect(page.locator(".composer .file-chip")).toHaveCount(2);
    await roomy(page);
    await page.getByLabel("Message or guidance").fill("with files");
    await page.getByRole("button", { name: "Send", exact: true }).click();
    const sent = page.locator(".transcript .message.user");
    await expect(sent.locator(".attachment-tile")).toBeVisible();
    await expect(sent.locator(".attachment-card")).toBeVisible();
    // The turn over: its controls are no longer held.
    await expect(page.locator(".transcript")).toContainText("Native worker");
    await expect(
      page.getByRole("button", { name: "Send", exact: true }),
    ).toBeVisible();
    await roomy(page);
  });

  test("a question's options", async ({ page, session, request }) => {
    await servePending(page, request, session, SAMPLE_ASK);
    await page.goto(`/#session=${session.id}`);
    await expect(
      page.getByRole("region", { name: "Pending decision" }),
    ).toBeVisible();
    await roomy(page);
  });

  test("a side chat's way back, and its row in Activity", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    await online(page);
    await page.getByLabel("Message or guidance").fill("/btw Side tool probe");
    await page.getByRole("button", { name: "Send", exact: true }).click();
    await page.getByRole("button", { name: "Allow once", exact: true }).click();
    await expect(page.locator(".transcript")).toContainText(
      "SIDE-TOOL-COMPLETE",
    );
    // The turn over: Stop is no longer there to press.
    await expect(
      page.getByRole("button", { name: "Send", exact: true }),
    ).toBeVisible();
    await roomy(page);
    // A tap on the way back is not taken by the totals beside it.
    await page
      .locator(".conversation-head")
      .getByRole("button", { name: /^‹/ })
      .tap();
    await expect(page).toHaveURL(new RegExp(`session=${session.id}$`));
    await page.locator(".composer .activity-button").tap();
    await expect(page.getByRole("dialog", { name: "Activity" })).toBeVisible();
    await roomy(page);
  });

  test("the browser", async ({ page, session }) => {
    const { serveFramebuffer } = await import("./rfb-fixture.js");
    const remote = await serveFramebuffer(page);
    await page.route("**/api/browser/status", (route) =>
      route.fulfill({
        json: {
          ok: true,
          display: 1,
          generation: 1,
          profile_id: "default",
          profiles: [{ id: "default", name: "Default" }],
          running: true,
          mode: "human",
          controller: true,
          leased: true,
        },
      }),
    );
    await page.goto(`/#session=${session.id}`);
    await remote.prepare();
    await headerAction(page, "Open browser");
    await expect(
      page.getByRole("button", { name: /^You're driving/ }),
    ).toBeVisible();
    await roomy(page);
  });

  test("This conversation's context, its lists open", async ({
    page,
    session,
  }) => {
    await page.goto(`/#session=${session.id}`);
    await page
      .getByRole("button", { name: "Context and usage", exact: true })
      .click();
    const sheet = page.getByRole("dialog", { name: "Settings" });
    await expect(sheet.getByRole("img", { name: /compacts at/ })).toBeVisible();
    for (const name of [/^Tools \d+/, /^Not offered/]) {
      const fold = sheet.getByRole("button", { name });
      if (await fold.isEnabled()) await fold.click();
    }
    await expect(sheet.locator(".context-tools").first()).toBeVisible();
    await roomy(page);
  });

  test("the tools", async ({ page, session }) => {
    await page.goto(`/#session=${session.id}`);
    await page
      .locator(".conversation-head")
      .getByRole("button", { name: "Conversation menu" })
      .click();
    await page.getByRole("menuitem", { name: "Tools", exact: true }).click();
    await expect(page.locator(".tool-toggle").first()).toBeVisible();
    await roomy(page);
  });

  test("the providers form", async ({ page, session }) => {
    await page.goto(`/#session=${session.id}`);
    await headerAction(page, "Settings");
    await page
      .locator(".settings-nav")
      .getByRole("button", { name: "Models and providers", exact: true })
      .click();
    await page.getByRole("button", { name: "Add a provider" }).click();
    const adding = page.getByRole("dialog", { name: "Add a provider" });
    await adding
      .getByRole("button", { name: "OpenRouter", exact: true })
      .click();
    await roomy(page);
  });
});

// The probe itself: an opaque line on the edge is seen whatever its colour,
// a clear one is not.
test("the probe sees a black and a red line on the screen's edge", async ({
  page,
}) => {
  await page.goto("/");
  await page.locator("#app").evaluate((app) => {
    for (const [name, line] of [
      ["probe black", "border-bottom: 1px solid rgb(0, 0, 0); bottom: 0"],
      ["probe red", "border-top: 1px solid rgb(255, 0, 0); top: 0"],
      ["probe clear", "border-left: 1px solid rgba(0, 0, 0, 0); top: 0"],
    ]) {
      const box = document.createElement("div");
      box.setAttribute("aria-label", name);
      box.style.cssText = `position: fixed; inset-inline: 0; height: 8px; ${line}`;
      app.append(box);
    }
  });
  expect(
    (await cramped(page)).filter((found) => found.includes("probe")),
  ).toEqual([
    "a line along the screen's bottom edge: probe black",
    "a line along the screen's top edge: probe red",
  ]);
});

test.describe("a conversation in Scratch", () => {
  test.use({ places: true });

  test("the suggestion bar above the input", async ({
    page,
    host,
    request,
  }) => {
    const { id } = await unfiled(host, request, "Three days in Lisbon", {
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

test.describe("a narrow list", () => {
  test.use({ isMobile: false, hasTouch: false });

  test("keeps a conversation's title before its place, and its foot inside", async ({
    page,
    host,
    request,
  }) => {
    await savedConversation(host, request, "Run exactly this command once");
    await page.setViewportSize({ width: 1000, height: 700 });
    await page.goto("/");
    await page.addStyleTag({ content: ".sidebar { --sidebar-width: 200px }" });
    const row = page.locator(".session-row").first();
    await expect(row).toBeVisible();
    const [title, place] = await row.evaluate((node) =>
      [".row-label", ".session-tag, .session-time"].map(
        (part) => node.querySelector(part).getBoundingClientRect().width,
      ),
    );
    expect(title).toBeGreaterThan(2 * place);
    await roomy(page);
  });
});
