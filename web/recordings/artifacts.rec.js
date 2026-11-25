// The manual's stills of each kind of artifact: a design system, a design, a
// sheet, a deck and a document, each a plain HTML file in the folder.
import {
  artboard,
  expect,
  online,
  paper,
  sheet,
  test,
  viewerOf,
  writeFiles,
} from "../tests/fixtures.js";

test.use({
  viewport: { width: 1480, height: 860 },
  deviceScaleFactor: 1.5,
  colorScheme: "light",
  video: "off",
});

const tokens = `:root {
  --page: #ffffff; --surface: #fafafa; --ink: #111111; --muted: #555555;
  --line: #cccccc; --accent: #000000; --good: #1a7f37; --bad: #b91c1c;
  --font: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  --space: 8px; --control: 32px;
}
* { box-sizing: border-box }
body { margin: 0; background: var(--page); color: var(--ink); font: 14px/1.5 var(--font) }
h1, h2, p { margin: 0 }
.button { display: inline-flex; align-items: center; height: var(--control); padding: 0 12px; background: #ededed }
.button.primary { background: var(--accent); color: var(--page) }
.line { border: 1px solid var(--line) }
`;
const linked = (path) => `<link rel="stylesheet" href="${path}">`;
const swatch = (name, value) =>
  `<div><div class="line" style="height:64px;background:var(${name})"></div><b>${name}</b><br>${value}</div>`;
const book = paper(
  "µAgent design system",
  `<section style="padding:18mm;display:grid;gap:10mm">
  <header><h1 style="font-size:34px">µAgent design system</h1><p style="color:var(--muted)">One set of tokens in tokens.css. Every page links it.</p></header>
  <div><h2>Colour</h2><div style="display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-top:10px">
    ${swatch("--page", "#ffffff")}${swatch("--surface", "#fafafa")}${swatch("--ink", "#111111")}${swatch("--muted", "#555555")}
    ${swatch("--line", "#cccccc")}${swatch("--accent", "#000000")}${swatch("--good", "#1a7f37")}${swatch("--bad", "#b91c1c")}
  </div></div>
  <div><h2>Type</h2><p style="font-size:28px;font-weight:700">Heading 28</p><p style="font-size:18px;font-weight:700">Title 18</p><p>Body 14, the size of every control and message.</p><p style="font-size:12px;color:var(--muted)">Detail 12</p></div>
  <div><h2>Controls</h2><p style="display:flex;gap:8px;margin-top:10px"><span class="button primary">Send</span><span class="button">Merge</span><span class="button line" style="background:none">Ask</span></p></div>
</section>`,
  "a4",
  linked("tokens.css"),
);

const frame = (title, size, body) =>
  artboard(
    title,
    `${linked("../system/tokens.css")}<div style="display:flex;flex-direction:column;height:100vh">${body}</div>`,
    size,
  );
const header = (count) =>
  `<header class="line" style="display:flex;align-items:center;gap:8px;padding:4px 8px;border-width:0 0 1px"><b style="flex:1">Verify the footprint numbers</b><span class="button primary">Side chats ${count}</span><span class="button">Terminal</span><span class="button">Files</span></header>`;
const column = `<aside class="line" style="width:36%;border-width:0 0 0 1px;padding:8px;display:grid;gap:8px;align-content:start"><p style="display:flex;gap:6px"><span class="button primary">Why streamed?</span><span class="button">Merge</span></p><p style="background:var(--ink);color:var(--page);padding:8px">Why streamed?</p><p>Each reply arrives as it is written.</p></aside>`;
const talk = `<main style="flex:1;padding:12px;display:grid;gap:8px;align-content:start"><p style="background:var(--ink);color:var(--page);padding:8px">Check the idle memory against the raw samples.</p><p><b>The published median holds: 27.4 MiB idle.</b></p></main>`;

const plan = sheet("Release 1.13", {
  "Pull requests": [
    '<col data-width="160"><col><col data-format="#,##0"><col data-format="#,##0"><col data-format="#,##0">',
    "<th>Area<th>Merged<th>Added<th>Removed<th>Net",
    "<td>MCP<td>4<td>3703<td>346<td>=C2-D2",
    "<td>Fixes<td>9<td>575<td>97<td>=C3-D3",
    "<td>Cuts<td>8<td>663<td>59236<td>=C4-D4",
    "<td>Docs, UI<td>4<td>3865<td>3198<td>=C5-D5",
    '<tr class="total"><td>Total<td>=SUM(B2:B5)<td>=SUM(C2:C5)<td>=SUM(D2:D5)<td>=C6-D6',
    '<figure data-chart="bar" data-range="A1:B5" data-at="A8" data-size="5x11"><figcaption>Pull requests by area</figcaption></figure>',
  ],
  "Before the tag": [
    '<col data-width="280"><col data-control="checkbox">',
    "<th>Check<th>Done",
    "<td>CI on master<td>TRUE",
    "<td>Coverage above 67%<td>TRUE",
    "<td>Both fuzzers, five minutes<td>TRUE",
    "<td>Tag pushed<td>FALSE",
  ],
});

const deckStyle = `${tokens}
body { width: 1280px; height: 720px; padding: 64px; display: flex; flex-direction: column; gap: 24px }
h1 { font-size: 72px; line-height: 1.05 }
h2 { font-size: 44px }
li { font-size: 28px; margin: 12px 0 }
.bar { height: 44px; background: var(--ink) }
`;
const page = (title, body, notes = "") =>
  artboard(title, `${body}<aside>${notes}</aside>`, "1280x720 slide").replace(
    "</head>",
    `${linked("deck.css")}</head>`,
  );
const notes = paper(
  "µAgent 1.13",
  `<style>${tokens} body { padding: 0 } h1 { font-size: 30px } h2 { font-size: 18px; margin-top: 18px } td, th { border: 1px solid var(--line); padding: 4px 8px; text-align: left } table { border-collapse: collapse; width: 100% }</style>
  <h1>µAgent 1.13</h1><p style="color:var(--muted)">Release notes, 25 pull requests since 1.12.</p>
  <h2>MCP</h2><p>A server can ask for input while a tool runs, answer a call with a task that runs in the background, and serve skills. Each served skill asks before it loads.</p>
  <h2>Fixes</h2><ul class="checklist"><li data-done>A site rule follows no redirect to another host</li><li data-done>A save and the agent's write share one lock</li><li data-done>Eight pages keep their live connection</li><li>Tag the release</li></ul>
  <h2>Size</h2><table><tr><th>Area</th><th>Lines removed</th></tr><tr><td>Vendored libraries</td><td>48,225</td></tr><tr><td>Comparison benchmarks</td><td>8,533</td></tr><tr><td>Repeated tests</td><td>1,019</td></tr></table>`,
  "a4",
);

test("artifacts", async ({ page: app, session, host }) => {
  test.setTimeout(120000);
  await writeFiles(host.project, {
    "design/system/tokens.css": tokens,
    "design/system/book.html": book,
    "design/side-chats/01-desktop.html": frame(
      "Side chats, desktop",
      "1280x800",
      `${header(2)}<div style="display:flex;flex:1">${talk}${column}</div>`,
    ),
    "design/side-chats/01-phone.html": frame(
      "Side chats, phone",
      "390x800",
      `${header(2).replace('<span class="button">Terminal</span>', "")}${talk}`,
    ),
    "plans/release.html": plan,
    "decks/release/deck.css": deckStyle,
    "decks/release/01-title.html": page(
      "µAgent 1.13",
      "<p>RELEASE</p><h1>µAgent 1.13</h1><h2>MCP servers ask, run and teach</h2>",
      "Twenty-five pull requests since 1.12.",
    ),
    "decks/release/02-mcp.html": page(
      "Three MCP extensions",
      "<h2>Three MCP extensions</h2><ul><li>Elicitation: a server asks you, on a card</li><li>Tasks: a call keeps running while the agent works</li><li>Skills: served, checked by digest, loaded on your word</li></ul>",
    ),
    "decks/release/03-size.html": page(
      "57,000 lines lighter",
      '<h2>57,000 lines lighter</h2><div class="bar" style="width:85%"></div><p>vendored libraries</p><div class="bar" style="width:15%"></div><p>benchmarks</p>',
    ),
    "docs/release-notes.html": notes,
  });
  // The pane takes most of the window. A folder is opened from the list.
  const shot = async (name, path, ready) => {
    const folder = path.endsWith("/");
    const files = app.getByRole("region", { name: "Files" });
    if (folder) {
      await files.getByRole("button", { name: "Back to the list" }).click();
      for (const part of path.split("/").filter(Boolean)) {
        const row = files.getByRole("button", { name: part, exact: true });
        if ((await row.getAttribute("aria-expanded")) !== "true")
          await row.click();
      }
      await files
        .locator(`.tree-folder:has([data-path="${path.slice(0, -1)}"])`)
        .getByRole("button", { name: "Open", exact: true })
        .click();
    } else {
      await app.goto(`/#session=${session.id}&file=${path}`);
      await online(app);
    }
    await ready(files, viewerOf(files).stage);
    // Wide, however the pane was left.
    await expect(async () => {
      const edge = await app
        .getByRole("separator", { name: "Resize the files" })
        .boundingBox();
      await app.mouse.move(edge.x + edge.width / 2, 400);
      await app.mouse.down();
      await app.mouse.move(430, 400, { steps: 6 });
      await app.mouse.up();
      expect((await files.boundingBox()).width).toBeGreaterThan(800);
    }).toPass();
    await app.mouse.move(0, 0);
    // A drag that missed the edge selects words.
    await app.evaluate(() => getSelection().removeAllRanges());
    await app.evaluate(() => document.fonts.ready);
    await files.screenshot({
      path: `../docs/images/artifact-${name}.png`,
      animations: "disabled",
    });
  };
  await shot("design-system", "design/system/book.html", (_, stage) =>
    expect(stage.getByText("µAgent design system").first()).toBeVisible(),
  );
  await shot("design", "design/side-chats/", (files) =>
    expect(
      files.locator('iframe[title="Side chats, desktop"]').first(),
    ).toBeVisible(),
  );
  await shot("sheet", "plans/release.html", (_, stage) =>
    expect(stage.getByText("59,236")).toBeVisible(),
  );
  await shot("deck", "decks/release/01-title.html", async (files, stage) => {
    await expect(
      stage.getByRole("heading", { name: "µAgent 1.13" }),
    ).toBeVisible();
  });
  await shot("document", "docs/release-notes.html", (_, stage) =>
    expect(stage.getByRole("heading", { name: "µAgent 1.13" })).toBeVisible(),
  );
});
