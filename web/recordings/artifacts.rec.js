// The manual's stills of each kind of artifact: a design system, a design, a
// sheet, a deck and a document, each a plain HTML file in the folder.
import { copyFile, mkdir } from "node:fs/promises";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
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
import { markPath } from "../src/shared/mark.ts";

test.use({
  viewport: { width: 1480, height: 860 },
  deviceScaleFactor: 1.5,
  colorScheme: "light",
  video: "off",
});

const face = "jetbrains-mono-latin-wght-normal.woff2";
const tokens = `@font-face {
  font-family: "JetBrains Mono"; font-weight: 100 800; font-display: block;
  src: url("fonts/${face}") format("woff2-variations");
}
:root, [data-theme="light"] {
  --bg: #fff; --surface: #fafafa; --raised: #eee; --hover: #e2e2e2; --press: #d4d4d4;
  --text: #111; --muted: #555; --line: #ccc; --field-border: #767676;
  --accent: #000; --on-accent: #fff;
  --live: #1a7f37; --added: #116329; --removed: #b31d28; --warning: #855600;
}
[data-theme="dark"] {
  --bg: #000; --surface: #0a0a0a; --raised: #171717; --hover: #232323; --press: #2e2e2e;
  --text: #f5f5f5; --muted: #a3a3a3; --line: #404040; --field-border: #8a8a8a;
  --accent: #fff; --on-accent: #000;
  --live: #22a35a; --added: #3fb950; --removed: #f85149; --warning: #e3b341;
}
:root {
  --font: "JetBrains Mono", ui-monospace, Menlo, Consolas, monospace;
  --space-2: 4px; --space-4: 8px; --space-6: 12px; --space-12: 24px;
  --control: 32px; --touch: 44px;
}
* { box-sizing: border-box }
body { margin: 0; background: var(--bg); color: var(--text); font: 14px/1.5 var(--font) }
h1, h2, p { margin: 0 }
.button { display: inline-flex; align-items: center; gap: 6px; height: var(--control); padding: 0 var(--space-6); background: var(--raised); color: var(--text) }
.button.primary { background: var(--accent); color: var(--on-accent) }
.button.quiet { background: none }
.line { border: 1px solid var(--line) }
`;
const linked = (path) => `<link rel="stylesheet" href="${path}">`;
const swatch = (name, value) =>
  `<div><i style="background:var(--${name})"></i><b>--${name}</b><span>${value}</span></div>`;
const bookStyle = `
body { font-size: 11px }
main { display: grid; gap: 4.5mm }
.cover { display: flex; align-items: flex-end; gap: 18px; padding-bottom: 5mm }
.cover svg { width: 96px; height: 96px; margin: -18px 0 -12px -24px }
.cover h1 { font-size: 40px; line-height: 1; margin-bottom: 6px }
.cover p { margin-left: auto; text-align: right; color: var(--muted) }
section { display: grid; grid-template-columns: 24mm minmax(0, 1fr); gap: 5mm; padding-top: 3mm; border-top: 1px solid var(--text) }
section > h2 { font-size: 11px; letter-spacing: 0.08em; text-transform: uppercase }
section > h2 small { display: block; color: var(--muted); font-weight: 400; letter-spacing: 0; text-transform: none }
.swatches { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 10px 8px }
.swatches i { display: block; height: 28px; border: 1px solid var(--line) }
.swatches b, .swatches span { display: block; font-size: 9.5px; line-height: 1.35 }
.swatches span { color: var(--muted) }
.type { display: grid; grid-template-columns: auto minmax(0, 1fr); gap: 2px 14px; align-items: baseline }
.type span { color: var(--muted) }
.glyphs { grid-row: span 4; font-size: 64px; font-weight: 700; line-height: 0.9; letter-spacing: -0.04em }
.scale { display: flex; align-items: flex-end; gap: 10px }
.scale div { display: grid; gap: 4px; justify-items: center; color: var(--muted); font-size: 9.5px }
.scale i { display: block; background: var(--text) }
.controls { display: flex; flex-wrap: wrap; gap: 8px; align-items: center }
.field { display: inline-flex; align-items: center; height: var(--control); padding: 0 var(--space-4); width: 118px; border: 1px solid var(--field-border); background: var(--raised); color: var(--muted) }
.tabs { display: inline-flex } .tabs .button { background: var(--raised) } .tabs .on { background: var(--accent); color: var(--on-accent) }
pre { margin: 0; padding: 10px; background: var(--raised); font: inherit; white-space: pre-wrap }
.other { padding: 10px; display: flex; gap: 8px; align-items: center; background: var(--bg); color: var(--text) }
.rules { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 12px }
.rules b { display: block }
`;
const bar = (px, label = px) =>
  `<div><i style="width:${px}px;height:${px}px"></i>${label}</div>`;
const controls = `<span class="button primary">Send</span><span class="button">Merge</span><span class="button quiet">Cancel</span><span class="tabs"><span class="button on">Recent</span><span class="button">Places</span></span>`;
// The app's own mark, as its sidebar draws it. Dark is the theme it opens in.
const book = `<!doctype html><html data-theme="dark"><head><title>µAgent design system</title>
<meta name="print" content="flow a4">${linked("tokens.css")}<style>${bookStyle}</style></head><body><main>
  <header class="cover"><svg viewBox="0 0 16 16" role="img" aria-label="µAgent"><path fill="currentColor" d="${markPath}"/></svg><div><h1>µAgent</h1>Design system</div><p>tokens.css<br>dark and light</p></header>
  <section><h2>Colour<small>surfaces, ink, signals</small></h2><div class="swatches">
    ${swatch("bg", "#000")}${swatch("surface", "#0a0a0a")}${swatch("raised", "#171717")}${swatch("hover", "#232323")}${swatch("press", "#2e2e2e")}
    ${swatch("text", "#f5f5f5")}${swatch("muted", "#a3a3a3")}${swatch("line", "#404040")}${swatch("field-border", "#8a8a8a")}${swatch("accent", "#fff")}
    ${swatch("live", "#22a35a")}${swatch("added", "#3fb950")}${swatch("removed", "#f85149")}${swatch("warning", "#e3b341")}
  </div></section>
  <section><h2>Type<small>JetBrains Mono</small></h2><div class="type">
    <b class="glyphs">Aaµ</b><b style="font-size:17px">Display 1.2rem</b>
    <b style="font-size:15.4px">Title 1.1rem</b>
    <span style="font-size:14px;color:var(--text)">Body 1rem, messages and controls</span>
    <span style="font-size:12px">Small 12px, times and totals</span>
  </div></section>
  <section><h2>Space<small>steps of 2</small></h2><div class="scale">${[2, 4, 6, 8, 12, 24, 48].map((px) => bar(px)).join("")}</div></section>
  <section><h2>Controls<small>32 tall, 44 on touch</small></h2><div style="display:grid;gap:10px">
    <div class="controls">${controls}<span class="field">Ask µAgent…</span></div>
    <div class="other" data-theme="light">${controls}<span class="field">Ask µAgent…</span></div>
  </div></section>
  <section><h2>Rules</h2><div class="rules">
    <p><b>Square and flat.</b>No radius, no shadow, one line weight.</p>
    <p><b>Even insets.</b>A control is as far from its box sideways as up and down.</p>
    <p><b>Two buttons.</b>One primary per view; the rest are secondary or quiet.</p>
  </div></section>
  <section><h2>Use<small>a page links the tokens</small></h2><pre>&lt;link rel="stylesheet" href="../system/tokens.css"&gt;
&lt;span class="button primary"&gt;Send&lt;/span&gt;</pre></section>
</main></body></html>`;

const frame = (title, size, body) =>
  artboard(
    title,
    `${linked("../system/tokens.css")}<div style="display:flex;flex-direction:column;height:100vh">${body}</div>`,
    size,
  );
const header = (count) =>
  `<header class="line" style="display:flex;align-items:center;gap:8px;padding:4px 8px;border-width:0 0 1px"><b style="flex:1">Verify the footprint numbers</b><span class="button primary">Side chats ${count}</span><span class="button">Terminal</span><span class="button">Files</span></header>`;
const column = `<aside class="line" style="width:36%;border-width:0 0 0 1px;padding:8px;display:grid;gap:8px;align-content:start"><p style="display:flex;gap:6px"><span class="button primary">Why streamed?</span><span class="button">Merge</span></p><p style="background:var(--text);color:var(--bg);padding:8px">Why streamed?</p><p>Each reply arrives as it is written.</p></aside>`;
const talk = `<main style="flex:1;padding:12px;display:grid;gap:8px;align-content:start"><p style="background:var(--text);color:var(--bg);padding:8px">Check the idle memory against the raw samples.</p><p><b>The published median holds: 27.4 MiB idle.</b></p></main>`;

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
.bar { height: 44px; background: var(--text) }
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
  await mkdir(join(host.project, "design/system/fonts"), { recursive: true });
  await copyFile(
    fileURLToPath(
      new URL(
        `../node_modules/@fontsource-variable/jetbrains-mono/files/${face}`,
        import.meta.url,
      ),
    ),
    join(host.project, "design/system/fonts", face),
  );
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
    expect(stage.getByText("Even insets.")).toBeVisible(),
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
