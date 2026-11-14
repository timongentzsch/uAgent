// The header's Files button shows the conversation's folder beside the chat
// (a sheet on a small screen): a list with search, and the open file.
import { execFileSync } from "node:child_process";
import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import {
  test,
  expect,
  artboard,
  headerAction,
  openFiles,
  openSession,
  saveHistory,
  savedConversation,
  selectProbeModel,
  slide,
  viewerOf,
  writeFiles,
} from "./fixtures.js";

const folder = (project) =>
  writeFiles(project, {
    "src/app.ts":
      "// One.\nexport const two = 2;\nexport const three = `3\n4`;\n",
    "README.md": "# Demo\n\n![The shot](shot.png) See [the app](src/app.ts).\n",
    "blob.bin": Buffer.from([0, 1, 2, 0, 255]),
  });

// design/landing: a design of two pages, each titled and headed by its name.
const landing = (project, names = ["hero-a", "hero-b"]) =>
  writeFiles(
    join(project, "design/landing"),
    Object.fromEntries(
      names.map((name) => [`${name}.html`, artboard(name, `<h1>${name}</h1>`)]),
    ),
  );

async function repository(project) {
  await folder(project);
  const git = (...args) =>
    execFileSync(
      "git",
      ["-c", "user.name=T", "-c", "user.email=t@example.com", ...args],
      { cwd: project },
    );
  git("init", "-q");
  git("add", ".");
  git("commit", "-q", "-m", "first");
  await writeFile(
    join(project, "src", "app.ts"),
    "// One.\nexport const two = 3;\n",
  );
  await writeFile(join(project, "added.txt"), "new\n");
}

// Opening or closing the pane narrows the chat in place: the transcript
// keeps its element, its scroll and its history, never loading them again.
test("the files pane does not remount the chat", async ({ page, session }) => {
  await openSession(page, session);
  const transcript = page.locator(".transcript");
  await expect(transcript).toBeVisible();
  await transcript.evaluate((element) => {
    window.keptTranscript = element;
  });
  const same = () =>
    page.evaluate(
      () => document.querySelector(".transcript") === window.keptTranscript,
    );
  await headerAction(page, "Files");
  await expect(page.getByRole("region", { name: "Files" })).toBeVisible();
  expect(await same()).toBe(true);
  await headerAction(page, "Files");
  await expect(page.getByRole("region", { name: "Files" })).toHaveCount(0);
  expect(await same()).toBe(true);
});

test("files beside the chat: browse, read, search and view", async ({
  page,
  session,
  host,
}) => {
  await folder(host.project);
  const pane = await openFiles(page, session);
  const list = pane.getByRole("navigation", { name: "Folder" });
  // Folders first.
  await expect(list.getByRole("button").nth(1)).toHaveText("src");

  await list.getByRole("button", { name: "src" }).click();
  await list.getByRole("button", { name: "app.ts" }).click();
  await expect(pane.getByRole("heading")).toHaveText("src/app.ts");
  // One numbered row per line; the template string spans two.
  await expect(pane.locator(".code-view .line-number")).toHaveCount(4);
  await expect(pane.locator(".code-view .line").nth(1)).toHaveText(
    "export const two = 2;",
  );

  // Markdown is rendered; its image and link resolve in its folder.
  await list.getByRole("button", { name: "README.md" }).click();
  await expect(pane.locator(".markdown [data-level='1']")).toHaveText("Demo");
  await expect(pane.getByRole("img", { name: "The shot" })).toHaveJSProperty(
    "naturalWidth",
    1,
  );
  await expect(pane.getByRole("button", { name: "Source" })).toHaveAttribute(
    "aria-pressed",
    "false",
  );
  await pane.getByRole("button", { name: "Source" }).click();
  await expect(pane.locator(".code-view .line-number")).toHaveCount(3);
  await pane.getByRole("button", { name: "Source" }).click();
  await pane.getByRole("link", { name: "the app" }).click();
  await expect(pane.getByRole("heading")).toHaveText("src/app.ts");

  // An image zooms where it lies, and Annotate opens it for markup; a file
  // that is not text offers a download.
  await list.getByRole("button", { name: "shot.png" }).click();
  const shot = pane.getByRole("group", { name: "shot.png" });
  await expect(shot.getByRole("img", { name: "shot.png" })).toBeVisible();
  await shot.getByRole("button", { name: "Zoom in" }).click();
  await expect(shot.getByRole("button", { name: "Fit" })).toBeEnabled();
  await pane.getByRole("button", { name: "Annotate" }).click();
  await expect(page.locator("dialog.image-view")).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(page.locator("dialog.image-view")).toHaveCount(0);
  await list.getByRole("button", { name: "blob.bin" }).click();
  await expect(pane.getByText("blob.bin is not text")).toBeVisible();

  // Search reaches into folders; Enter opens the active match.
  const search = pane.getByRole("combobox", { name: "Search files" });
  await search.fill("apts");
  await expect(pane.getByRole("option")).toHaveText(["app.tssrc"]);
  await search.press("Enter");
  await expect(pane.getByRole("heading")).toHaveText("src/app.ts");

  await pane.getByRole("button", { name: "Close files" }).click();
  await expect(pane).toHaveCount(0);
});

test("on a phone files are a sheet showing the list or the file", async ({
  page,
  session,
  host,
}) => {
  await folder(host.project);
  const sheet = await openFiles(page, session, "", { phone: true });
  const list = sheet.getByRole("navigation", { name: "Folder" });
  await list.getByRole("button", { name: "README.md" }).click();
  await expect(sheet.locator(".markdown [data-level='1']")).toHaveText("Demo");
  await expect(list).toBeHidden();
  await sheet.getByRole("button", { name: "Back to the list" }).click();
  await expect(list).toBeVisible();
  // No pane to resize on a small screen.
  await expect(page.getByRole("separator")).toHaveCount(0);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth - window.innerWidth,
    ),
  ).toBeLessThanOrEqual(0);
});

test("docked panes resize by drag and keys, and keep their width", async ({
  page,
  session,
}) => {
  await openSession(page, session);
  const sidebar = page.getByRole("complementary", {
    name: "Folders and conversations",
  });
  const width = async (locator) => (await locator.boundingBox()).width;
  const before = await width(sidebar);
  const edge = page.getByRole("separator", { name: "Resize the sidebar" });
  await edge.focus();
  await edge.press("ArrowRight");
  await edge.press("Shift+ArrowRight");
  expect(await width(sidebar)).toBeCloseTo(before + 80, 0);
  await page.reload();
  await expect.poll(() => width(sidebar)).toBeCloseTo(before + 80, 0);
  await edge.press("Enter");
  expect(await width(sidebar)).toBeCloseTo(before, 0);

  // The files pane widens when its edge is dragged left, and stops short
  // of crowding out the chat.
  await headerAction(page, "Files");
  const pane = page.locator(".side-pane");
  const handle = page.getByRole("separator", { name: "Resize the files" });
  const start = await width(pane);
  const box = await handle.boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + 100);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width / 2 - 120, box.y + 100, { steps: 4 });
  await page.mouse.up();
  expect(await width(pane)).toBeCloseTo(start + 120, 0);
  await page.mouse.move(box.x - 120, box.y + 100);
  await page.mouse.down();
  await page.mouse.move(0, box.y + 100, { steps: 4 });
  await page.mouse.up();
  const chat = await width(page.locator(".side-chat"));
  expect(chat).toBeGreaterThanOrEqual(355);
  expect(chat).toBeLessThan(365);
});

test("changes list what differs from the last commit and open as a diff", async ({
  page,
  session,
  host,
}) => {
  await repository(host.project);
  const pane = await openFiles(page, session);
  const list = pane.getByRole("navigation", { name: "Folder" });
  const toggle = list.getByRole("button", { name: "Changes 2" });
  await expect(toggle).toHaveAttribute("aria-pressed", "false");
  await toggle.click();
  await expect(toggle).toHaveAttribute("aria-pressed", "true");
  await expect(list.locator(".tree-row")).toHaveText([
    "Msrc/app.ts",
    "Aadded.txt",
  ]);
  await list.getByRole("button", { name: "src/app.ts" }).click();
  await expect(pane.locator(".diff-add")).toHaveText("+export const two = 3;");
  await expect(pane.locator(".files-head")).toContainText("+1 −3");
  await list.getByRole("button", { name: "Files", exact: true }).click();
  await expect(list.getByRole("button", { name: "src" })).toBeVisible();
});

test("on a phone a change opens as a diff with Back to the list", async ({
  page,
  session,
  host,
}) => {
  await repository(host.project);
  const sheet = await openFiles(page, session, "", { phone: true });
  const list = sheet.getByRole("navigation", { name: "Folder" });
  await list.getByRole("button", { name: "Changes 2" }).click();
  await list.getByRole("button", { name: "added.txt" }).click();
  await expect(sheet.locator(".diff-add")).toHaveText("+new");
  await expect(list).toBeHidden();
  await sheet.getByRole("button", { name: "Back to the list" }).click();
  await expect(list).toBeVisible();
});

test("a folder that is not a repository has no changes toggle", async ({
  page,
  session,
  host,
}) => {
  await folder(host.project);
  const pane = await openFiles(page, session);
  await expect(pane.getByRole("button", { name: "README.md" })).toBeVisible();
  await expect(pane.getByRole("button", { name: /^Changes/ })).toHaveCount(0);
});

// A saved conversation in the project whose reply cites `reply`.
const citing = (host, request, reply) =>
  savedConversation(host, request, `Cites ${crypto.randomUUID()}`, reply);

test("a cited path opens the pane at its lines", async ({
  page,
  host,
  request,
}) => {
  await folder(host.project);
  const cited = await citing(
    host,
    request,
    "It is `src/app.ts:2`, also `app.ts:3`, [the start](src/app.ts#L1), `/tmp/notes.txt:3` and `ledger.summary`.",
  );
  await openSession(page, cited);
  const transcript = page.locator(".transcript");
  const pane = page.getByRole("region", { name: "Files" });
  const marked = pane.locator(".code-view .line[data-target]");

  const first = transcript.getByRole("button", { name: "src/app.ts:2" });
  await first.click();
  await expect(pane.getByRole("heading")).toHaveText("src/app.ts");
  await expect(marked).toHaveText(["export const two = 2;"]);
  await expect(marked).toBeInViewport();
  // The pane only shows the file: focus does not move into it.
  await expect(pane.locator(":focus")).toHaveCount(0);

  // A bare name finds the one file it names.
  await transcript.getByRole("button", { name: "app.ts:3" }).click();
  await expect(marked).toHaveText(["export const three = `3"]);
  // A link to a path is a reference too.
  await transcript.getByRole("button", { name: "the start" }).click();
  await expect(marked).toHaveText(["// One."]);
  // Not a file: plain code.
  await expect(
    transcript.getByRole("button", { name: "ledger.summary" }),
  ).toHaveCount(0);
  // Outside the folder the pane says so.
  await transcript.getByRole("button", { name: "/tmp/notes.txt:3" }).click();
  await expect(
    pane.getByText("/tmp/notes.txt is outside this conversation's folder"),
  ).toBeVisible();

  // Enter on a focused reference opens it too.
  await first.focus();
  await page.keyboard.press("Enter");
  await expect(marked).toHaveText(["export const two = 2;"]);
});

test("a name several files share lists them, and the pick opens at the line", async ({
  page,
  host,
  request,
}) => {
  await folder(host.project);
  await writeFiles(host.project, { "lib/app.ts": "one\ntwo\n" });
  const cited = await citing(host, request, "See `app.ts:2`.");
  await openSession(page, cited);
  await page
    .locator(".transcript")
    .getByRole("button", { name: "app.ts:2" })
    .click();
  const pane = page.getByRole("region", { name: "Files" });
  await expect(
    pane.getByText("2 files are named app.ts. Pick one to open it at line 2."),
  ).toBeVisible();
  await pane.getByRole("option", { name: /lib/ }).click();
  await expect(pane.getByRole("heading")).toHaveText("lib/app.ts");
  await expect(pane.locator(".code-view .line[data-target]")).toHaveText([
    "two",
  ]);
});

test("on a phone a cited path opens the sheet at its lines", async ({
  page,
  host,
  request,
}) => {
  await folder(host.project);
  const cited = await citing(
    host,
    request,
    "It is `src/app.ts:2`, pinned by `tests/unit/tool_execution_policy_test.cc:130` and `benchmarks/baselines/hermetic.json`.",
  );
  await openSession(page, cited, { phone: true });
  // A long cited path wraps like any code instead of widening the chat.
  const transcript = page.locator(".transcript");
  await expect(transcript.locator(".file-ref").last()).toBeVisible();
  expect(
    await transcript.evaluate((node) => node.scrollWidth - node.clientWidth),
  ).toBeLessThanOrEqual(0);
  await page
    .locator(".transcript")
    .getByRole("button", { name: "src/app.ts:2" })
    .click();
  const sheet = page.getByRole("dialog", { name: "Files" });
  await expect(sheet.locator(".code-view .line[data-target]")).toHaveText([
    "export const two = 2;",
  ]);
  await expect(sheet.getByRole("navigation", { name: "Folder" })).toBeHidden();
  // It opens on the file: the list never shows, so nothing slides across.
  await expect(sheet.locator(".push")).toHaveCount(0);
});

test("lines selected in a file go to the chat as a reference", async ({
  page,
  session,
  host,
}) => {
  await folder(host.project);
  const pane = await openFiles(page, session);
  const list = pane.getByRole("navigation", { name: "Folder" });
  await list.getByRole("button", { name: "src" }).click();
  await list.getByRole("button", { name: "app.ts" }).click();
  const add = pane.getByRole("button", { name: "Add to chat" });
  await expect(add).toHaveCount(0);
  await pane.locator(".code-view").evaluate((view) => {
    const lines = view.querySelectorAll(".line");
    const range = document.createRange();
    range.setStart(lines[0].firstChild, 0);
    range.setEnd(lines[1].firstChild, 1);
    getSelection().removeAllRanges();
    getSelection().addRange(range);
  });
  await add.click();
  await expect(page.getByLabel("Message or guidance")).toHaveValue(
    "`src/app.ts:1-2`",
  );
});

test("what the agent presents shows as a card that opens it in Files", async ({
  page,
  host,
  session,
  command,
}) => {
  await landing(host.project);
  await writeFiles(
    join(host.project, "decks/talk"),
    Object.fromEntries(
      ["01-cover", "02-plan", "03-end"].map((name) => [
        `${name}.html`,
        slide(name),
      ]),
    ),
  );
  await selectProbeModel(command, session, { yolo: true });
  await openSession(page, session);
  const prompt = page.getByLabel("Message or guidance");
  const cards = page.locator(".transcript .artifact-card");
  await prompt.fill("Present probe design/landing decks/talk");
  await prompt.press("Enter");
  await expect(cards).toHaveCount(2);
  // A design: its first page live and small, and what it holds.
  await expect(cards.first()).toContainText("landing");
  await expect(cards.first()).toContainText("Design · 2 pages");
  await expect(
    cards.first().frameLocator('iframe[title="hero-a"]').getByText("hero-a"),
  ).toBeVisible();
  await cards
    .first()
    .getByRole("button", { name: "Open design/landing/" })
    .click();
  const pane = page.getByRole("region", { name: "Files" });
  await expect(pane.locator(".canvas-page")).toHaveCount(2);
  // A deck opens in the viewer, on its first slide.
  await expect(cards.last()).toContainText("Deck · 3 slides");
  await cards.last().getByRole("button", { name: "Open", exact: true }).click();
  await expect(pane.getByRole("listbox", { name: "Slides" })).toBeVisible();
  await expect(pane.getByRole("heading")).toHaveText(
    "decks/talk/01-cover.html",
  );
});

test("a page a reply names opens over its design's canvas", async ({
  page,
  host,
  request,
}) => {
  await landing(host.project);
  const cited = await citing(
    host,
    request,
    "The second hero is `design/landing/hero-b.html`.",
  );
  await openSession(page, cited);
  await page
    .locator(".transcript")
    .getByRole("button", { name: "design/landing/hero-b.html", exact: true })
    .click();
  const pane = page.getByRole("region", { name: "Files" });
  await expect(
    pane.frameLocator('iframe[title="hero-b"]').getByText("hero-b"),
  ).toBeVisible();
  await pane.getByRole("button", { name: "Back to the canvas" }).click();
  await expect(pane.locator(".canvas-page")).toHaveCount(2);
});

test("a page a reply names loosely opens by its name, never a blank frame", async ({
  page,
  host,
  request,
}) => {
  await writeFiles(host.project, {
    "design/system/components/Button/preview.html":
      "<!doctype html><title>Button</title><button>Save</button>",
  });
  const cited = await citing(
    host,
    request,
    "Each component has a `preview.html` you can switch between themes.",
  );
  await openSession(page, cited);
  const transcript = page.locator(".transcript");
  await expect(
    transcript.getByRole("button", { name: "preview.html", exact: true }),
  ).toBeVisible();
  await transcript
    .getByRole("button", { name: "preview.html", exact: true })
    .click();
  // Not at the top of the folder: found by its name, never a blank frame.
  const pane = page.getByRole("region", { name: "Files" });
  await expect(pane.getByRole("heading")).toHaveText(
    "design/system/components/Button/preview.html",
  );
  await expect(
    pane.frameLocator('iframe[title="Button"]').getByRole("button", {
      name: "Save",
    }),
  ).toBeVisible();
});

test("the folder is a tree: folders open in place, by click or keys, and stay open", async ({
  page,
  session,
  host,
}) => {
  await folder(host.project);
  const pane = await openFiles(page, session);
  const list = pane.getByRole("navigation", { name: "Folder" });
  const src = list.getByRole("button", { name: "src", exact: true });
  await expect(src).toHaveAttribute("aria-expanded", "false");
  // A folder opens in place: its files appear beneath it, the rest stays.
  await src.click();
  await expect(src).toHaveAttribute("aria-expanded", "true");
  await expect(list.getByRole("button", { name: "app.ts" })).toBeVisible();
  await expect(list.getByRole("button", { name: "README.md" })).toBeVisible();
  // Left closes it; Right opens it again; Down walks into it.
  await src.focus();
  await page.keyboard.press("ArrowLeft");
  await expect(src).toHaveAttribute("aria-expanded", "false");
  await page.keyboard.press("ArrowRight");
  await expect(src).toHaveAttribute("aria-expanded", "true");
  await page.keyboard.press("ArrowDown");
  await expect(list.getByRole("button", { name: "app.ts" })).toBeFocused();
  // Closing and reopening the pane keeps what was open.
  await headerAction(page, "Files");
  await headerAction(page, "Files");
  await expect(
    pane.getByRole("navigation", { name: "Folder" }).getByRole("button", {
      name: "app.ts",
    }),
  ).toBeVisible();
  await list.getByRole("button", { name: "Collapse all" }).click();
  await expect(list.getByRole("button", { name: "app.ts" })).toHaveCount(0);
});

test("a cited design folder opens its canvas at once, with Files already open", async ({
  page,
  host,
  request,
}) => {
  await folder(host.project);
  await landing(host.project, ["01-hero", "02-pricing"]);
  const cited = await citing(host, request, "See `design/landing/`.");
  await openSession(page, cited);
  // Files shows the folder's top: its listing is loaded, and is not the
  // cited folder's.
  await headerAction(page, "Files");
  const pane = page.getByRole("region", { name: "Files" });
  await expect(
    pane
      .getByRole("navigation", { name: "Folder" })
      .getByRole("button", { name: "src" }),
  ).toBeVisible();
  await page
    .locator(".transcript .file-ref", { hasText: "design/landing/" })
    .click();
  await expect(pane.locator(".canvas-page")).toHaveCount(2);
});

// Two decks whose covers share a name, beside the folder of files.
async function decks(project) {
  await folder(project);
  await writeFiles(
    join(project, "decks"),
    Object.fromEntries(
      ["a/01-cover.html", "a/02-plan.html", "b/01-cover.html", "b/02-end.html"]
        // A slide is titled for its deck and its file: "a 01-cover.html".
        .map((path) => [path, slide(path.replace("/", " "))]),
    ),
  );
}

test("a cited name two files share asks which, even over another deck", async ({
  page,
  host,
  request,
}) => {
  await decks(host.project);
  const cited = await citing(
    host,
    request,
    "Deck `decks/a/` and its cover `01-cover.html`.",
  );
  await openSession(page, cited);
  const transcript = page.locator(".transcript");
  const pane = page.getByRole("region", { name: "Files" });
  // A cited deck opens at its first slide, its slides in the legend.
  const slide = viewerOf(pane).stage;
  await transcript.locator(".file-ref", { hasText: "decks/a/" }).click();
  await expect(
    slide.getByRole("heading", { name: "a 01-cover.html" }),
  ).toBeVisible();
  await expect(
    pane.getByRole("listbox", { name: "Slides" }).getByRole("option"),
  ).toHaveCount(2);
  await transcript.locator(".file-ref", { hasText: "01-cover.html" }).click();
  // The matches, never the deck or a blank page.
  await expect(pane.locator(".page-stage")).toHaveCount(0);
  await expect(
    pane.getByText("2 files are named 01-cover.html."),
  ).toBeVisible();
  const matches = pane.getByRole("listbox", { name: "Matching files" });
  await expect(matches.getByRole("option")).toHaveCount(2);
  await matches.getByRole("option", { name: /decks\/b/ }).click();
  await expect(
    slide.getByRole("heading", { name: "b 01-cover.html" }),
  ).toBeVisible();
  // Back returns to the matches.
  await pane.getByRole("button", { name: "Back to the matches" }).click();
  await expect(matches.getByRole("option")).toHaveCount(2);
});

test("the tree's Open shows that folder's deck, never the last one's", async ({
  page,
  session,
  host,
}) => {
  await decks(host.project);
  const pane = await openFiles(page, session);
  const list = pane.getByRole("navigation", { name: "Folder" });
  await list.getByRole("button", { name: "decks", exact: true }).click();
  // Each deck's own slides, from its first; Back is the deck's list.
  for (const deck of ["a", "b"]) {
    if (deck === "b")
      await pane.getByRole("button", { name: "Back to the list" }).click();
    await list.getByRole("button", { name: deck, exact: true }).click();
    await list
      .locator(`.tree-folder:has([data-path="decks/${deck}"])`)
      .getByRole("button", { name: "Open", exact: true })
      .click();
    await expect(pane.locator(".page-legend-name")).toHaveText([
      `1 · ${deck} 01-cover.html`,
      `2 · ${deck} ${deck === "a" ? "02-plan" : "02-end"}.html`,
    ]);
    await expect(
      viewerOf(pane).stage.getByRole("heading", {
        name: `${deck} 01-cover.html`,
      }),
    ).toBeVisible();
  }
});

test("closed and opened again, Files is where it was left", async ({
  page,
  host,
  request,
}) => {
  await decks(host.project);
  const cited = await citing(host, request, "See `decks/a/02-plan.html`.");
  await openSession(page, cited);
  await page
    .locator(".transcript .file-ref", { hasText: "decks/a/02-plan.html" })
    .click();
  const pane = page.getByRole("region", { name: "Files" });
  const slide = viewerOf(pane).stage.getByRole("heading", {
    name: "a 02-plan.html",
  });
  await expect(slide).toBeVisible();
  await pane.getByRole("button", { name: "Close files" }).click();
  await expect(pane).toHaveCount(0);
  await headerAction(page, "Files");
  await expect(slide).toBeVisible();
  await expect(
    pane.getByRole("option", { name: "Slide 2: a 02-plan.html" }),
  ).toHaveAttribute("aria-selected", "true");
  // Its way back is kept too: to its deck's list, a deck having no canvas.
  await pane.getByRole("button", { name: "Back to the list" }).click();
  await expect(
    pane.getByRole("button", { name: "01-cover.html", exact: true }),
  ).toBeVisible();
});

test("a cited folder that is not there says so, on the folder's top", async ({
  page,
  host,
  request,
}) => {
  await decks(host.project);
  const cited = await citing(host, request, "See `decks/gone/`.");
  await openSession(page, cited);
  await page
    .locator(".transcript .file-ref", { hasText: "decks/gone/" })
    .click();
  const pane = page.getByRole("region", { name: "Files" });
  await expect(
    pane.getByText("decks/gone/ is not in this folder."),
  ).toBeVisible();
  await expect(pane.locator(".canvas-page")).toHaveCount(0);
});

test("a deck stays open when the window narrows to a phone's sheet", async ({
  page,
  host,
  request,
}) => {
  await decks(host.project);
  const cited = await citing(host, request, "See `decks/a/`.");
  await openSession(page, cited);
  await page.locator(".transcript .file-ref", { hasText: "decks/a/" }).click();
  const slides = page.getByRole("listbox", { name: "Slides" });
  await expect(slides.getByRole("option")).toHaveCount(2);
  await page.setViewportSize({ width: 390, height: 844 });
  const sheet = page.getByRole("dialog", { name: "Files" });
  await expect(sheet.getByRole("option")).toHaveCount(2);
  await expect(
    viewerOf(sheet).stage.getByRole("heading", { name: "a 01-cover.html" }),
  ).toBeVisible();
});
