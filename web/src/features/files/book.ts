import { renderMarkdown } from "../../shared/markdown.ts";
import type { Tokens } from "./tokens.ts";

// A design system as a brand book: A4 pages set in the system's own colours
// and type, from its README, its tokens.css and its components. The same
// document shows in the pane and prints to PDF, one sheet a page.

export interface Component {
  name: string;
  path: string;
  group: string;
  height: number;
  // Its README's first paragraph.
  about: string;
}

const escape = (text: string) =>
  text.replace(
    /[&<>"]/g,
    (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[ch]!,
  );

// WCAG contrast of two #rgb or #rrggbb colours; null for anything else.
function contrast(a: string, b: string) {
  const rgb = (hex: string) => {
    const h = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(hex)?.[1];
    if (!h) return null;
    const full = h.length === 3 ? [...h].map((c) => c + c).join("") : h;
    return [0, 2, 4].map((at) => {
      const c = Number.parseInt(full.slice(at, at + 2), 16) / 255;
      return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
    });
  };
  const [x, y] = [rgb(a), rgb(b)];
  if (!x || !y) return null;
  const lum = ([r, g, bl]: number[]) => 0.2126 * r + 0.7152 * g + 0.0722 * bl;
  const [hi, lo] = [lum(x), lum(y)].sort((p, q) => q - p);
  return (hi + 0.05) / (lo + 0.05);
}

// The README cut at its second-level headings: its title, the paragraph
// under it, and each section.
function sections(readme: string) {
  const title = /^#\s+(.+)$/m.exec(readme)?.[1]?.trim() ?? "Design system";
  const parts = readme.split(/^##\s+/m);
  const lead = parts[0]
    .replace(/^#\s+.+$/m, "")
    .trim()
    .split(/\n\s*\n/)[0];
  return {
    title,
    lead: lead ?? "",
    parts: parts.slice(1).map((part) => {
      const [head, ...body] = part.split("\n");
      return { head: head.trim(), body: body.join("\n").trim() };
    }),
  };
}

export async function book({
  name,
  readme,
  tokens,
  components,
  sheet,
  page,
  date,
}: {
  // What the book is of, when its README's title says nothing more.
  name: string;
  readme: string;
  tokens: Tokens;
  components: Component[];
  // The address of the system's tokens.css, and of a file of its folder.
  sheet: string;
  page: (path: string) => string;
  date: string;
}) {
  const said = sections(readme);
  const { lead, parts } = said;
  const title = /^design system$/i.test(said.title) ? name : said.title;
  // README sections that a generated chapter tells better open that
  // chapter; "Not synced" ends the book; the rest are chapters of their own.
  const into: Record<string, RegExp> = {
    Colour: /^colou?rs?$/i,
    Typography: /^(type|typography)$/i,
    "Space and shape": /^(space|spacing|shape)$/i,
    Components: /^components$/i,
  };
  const intro = (chapter: string) =>
    parts.find((part) => into[chapter].test(part.head))?.body ?? "";
  // What the README says beside the tokens; "Not synced" ends the book.
  const told = parts.filter(
    (part) =>
      !/^not synced$/i.test(part.head) &&
      !Object.values(into).some((test) => test.test(part.head)),
  );
  const unsynced = parts.find((part) => /^not synced$/i.test(part.head));
  const theme = tokens.themes[0];
  const value = (name: string, at = theme): string => {
    const token = tokens.colors.find((color) => color.name === name);
    const raw = token ? (token.values[at] ?? token.value) : "";
    const alias = /^var\(--([\w-]+)\)$/.exec(raw)?.[1];
    return alias ? value(alias, at) : raw;
  };
  const ground = (at: string) =>
    ["bg", "page", "background", "surface"]
      .map((n) => value(n, at))
      .find(Boolean) ?? "";
  const chapters = [
    ...told.map((part) => part.head),
    ...(tokens.colors.length ? ["Colour"] : []),
    ...(tokens.text.length || tokens.scales.font.length ? ["Typography"] : []),
    ...(tokens.scales.space.length ||
    tokens.scales.radius.length ||
    tokens.scales.shadow.length
      ? ["Space and shape"]
      : []),
    ...(components.length ? ["Components"] : []),
  ];
  const number = (head: string) =>
    String(chapters.indexOf(head) + 1).padStart(2, "0");
  const opener = (head: string) =>
    `<header class="opener"><span class="no">${number(head)}</span><h2>${escape(head)}</h2></header>`;
  // A generated chapter's opening: its number and name, then what the
  // README says about it.
  const open = async (head: string) =>
    `${opener(head)}${intro(head) ? `<div class="prose intro">${await renderMarkdown(intro(head))}</div>` : ""}`;
  const pages: string[] = [];
  const add = (body: string, kind = "") =>
    pages.push(
      `<section class="page ${kind}">${body}<footer><span>${escape(title)}</span><span class="folio"></span></footer></section>`,
    );

  // Cover and contents.
  const strip = tokens.colors
    .slice(0, 8)
    .map((color) => `<i style="background:${escape(value(color.name))}"></i>`)
    .join("");
  add(
    `<div class="cover"><p class="kicker">Design system</p><h1>${escape(title)}</h1>` +
      `<div class="lead">${await renderMarkdown(lead)}</div><div class="strip">${strip}</div>` +
      `<p class="meta">${escape(date)}</p></div>`,
    "first",
  );
  add(
    `<h2 class="contents-title">Contents</h2><ol class="contents">${chapters
      .map(
        (head) =>
          `<li><span class="no">${number(head)}</span>${escape(head)}</li>`,
      )
      .join("")}</ol>`,
  );
  for (const part of told)
    add(
      `${opener(part.head)}<div class="prose">${await renderMarkdown(part.body)}</div>`,
      "flow",
    );

  // Colour: a page per theme, every swatch with its value, use and contrast.
  for (const at of tokens.themes) {
    const swatches = tokens.colors
      .map((color) => {
        const v = value(color.name, at);
        const ratio = contrast(v, ground(at));
        return `<figure class="swatch"><i style="background:${escape(v)}"></i><figcaption><b>${escape(color.name)}</b><code>${escape(v)}</code>${
          ratio ? `<span>${ratio.toFixed(1)}:1 on the ground</span>` : ""
        }<span>${escape(color.usage)}</span></figcaption></figure>`;
      })
      .join("");
    add(
      `${at === theme ? await open("Colour") : ""}<h3>${escape(at)} theme</h3><div class="swatches" data-theme="${escape(at)}">${swatches}</div>`,
      "flow",
    );
  }

  // Typography: each text style set large, with its rule, on the README's
  // own words as plain text.
  const sample =
    (lead || title)
      .replace(/\[([^\]]*)\]\([^)]*\)/g, "$1")
      .replace(/[`*_#>]/g, "")
      .slice(0, 140) || title;
  if (chapters.includes("Typography")) {
    const families = tokens.scales.font
      .map(
        (font) =>
          `<p class="family" style="font-family:${escape(font.value)}"><span>${escape(font.name)}</span>Aa Bb Cc 0123</p>`,
      )
      .join("");
    const styles = tokens.text
      .map(
        (style) =>
          `<div class="specimen"><p class="label">${escape(style.name)}<code>${escape(style.value)}</code></p><p style="${escape(style.value)}">${escape(sample)}</p></div>`,
      )
      .join("");
    add(`${await open("Typography")}${families}${styles}`, "flow");
  }

  // Space and shape: the scales drawn to size.
  if (chapters.includes("Space and shape")) {
    const space = tokens.scales.space
      .map(
        (token) =>
          `<p class="scale"><b>${escape(token.name)}</b><i style="width:${escape(token.value)}"></i><code>${escape(token.value)}</code><span>${escape(token.usage)}</span></p>`,
      )
      .join("");
    const shape = [...tokens.scales.radius, ...tokens.scales.shadow]
      .map((token) => {
        const css = token.name.startsWith("radius")
          ? `border-radius:${token.value}`
          : `box-shadow:${token.value}`;
        return `<figure class="tile"><i style="${escape(css)}"></i><figcaption><b>${escape(token.name)}</b><code>${escape(token.value)}</code></figcaption></figure>`;
      })
      .join("");
    add(
      `${await open("Space and shape")}${space}<div class="tiles">${shape}</div>`,
      "flow",
    );
  }

  // Components: each live, with what it is for.
  if (components.length)
    add(
      `${await open("Components")}${components
        .map(
          (c) =>
            `<article class="component"><p class="label">${escape(c.group || "Component")}</p><h3>${escape(c.name)}</h3><p>${escape(c.about)}</p><iframe src="${escape(page(c.path))}" title="${escape(c.name)}" sandbox="allow-scripts" style="height:${c.height}px"></iframe></article>`,
        )
        .join("")}`,
      "flow",
    );
  if (unsynced)
    add(
      `<h2 class="appendix">Not synced</h2><div class="prose small">${await renderMarkdown(unsynced.body)}</div>`,
      "flow",
    );

  // The book's ground, ink, accent and face, from the system's own values.
  const first = (names: string[]) =>
    names.map((n) => value(n)).find(Boolean) ?? "";
  const face =
    tokens.scales.font.find((font) => /[,"']/.test(font.value))?.value ?? "";
  const own = [
    ["--b-ground", ground(theme ?? "")],
    ["--b-ink", first(["text", "ink", "fg", "foreground"])],
    ["--b-muted", first(["muted", "text-muted", "secondary"])],
    ["--b-line", first(["line", "border", "divider"])],
    ["--b-accent", first(["accent", "primary", "brand"])],
    ["--b-face", face],
  ]
    .filter(([, v]) => v)
    .map(([k, v]) => `${k}: ${v};`)
    .join(" ");
  return `<!doctype html><html lang="en" data-theme="${escape(theme ?? "")}"><head><meta charset="utf-8"><title>${escape(title)}</title><link rel="stylesheet" href="${escape(sheet)}"><style>${STYLE} :root { ${escape(own)} }</style></head><body>${pages.join("")}</body></html>`;
}

// The book's own look, from the system's variables wherever it has them.
const STYLE = `
:root { --b-ground: Canvas; --b-ink: CanvasText; --b-muted: GrayText; --b-line: GrayText; --b-accent: CanvasText; --b-face: system-ui, sans-serif; --b-display: var(--b-face); }
@page { size: A4; margin: 0; }
* { box-sizing: border-box; }
html { background: color-mix(in srgb, var(--b-ink) 12%, var(--b-ground)); print-color-adjust: exact; -webkit-print-color-adjust: exact; }
body { margin: 0; padding: 24px 0; display: flex; flex-direction: column; align-items: center; gap: 24px; color: var(--b-ink); font: 400 10.5pt/1.55 var(--b-face); counter-reset: folio; }
.page { position: relative; width: 210mm; min-height: 297mm; padding: 22mm 20mm 26mm; background: var(--b-ground); counter-increment: folio; break-after: page; }
.page:not(.flow) { height: 297mm; overflow: hidden; }
.first { padding-bottom: 0; }
footer { position: absolute; left: 20mm; right: 20mm; bottom: 12mm; display: flex; justify-content: space-between; font-size: 8pt; letter-spacing: .06em; text-transform: uppercase; color: var(--b-muted); border-top: 1px solid var(--b-line); padding-top: 3mm; }
.folio::after { content: counter(folio, decimal-leading-zero); }
.first footer { display: none; }
.cover { height: 100%; display: flex; flex-direction: column; }
.kicker, .label { margin: 0; font-size: 8pt; letter-spacing: .14em; text-transform: uppercase; color: var(--b-muted); }
.cover h1 { margin: 60mm 0 10mm; font: 700 56pt/1 var(--b-display); letter-spacing: -.04em; }
.cover::before { content: ""; position: absolute; left: 20mm; top: 22mm; width: 14mm; height: 14mm; background: var(--b-accent); }
.cover .kicker { margin-top: 22mm; }
.cover .lead { max-width: 130mm; font-size: 12.5pt; line-height: 1.5; }
.cover .lead p { margin: 0 0 3mm; }
.strip { margin: auto -20mm 0; display: flex; height: 36mm; }
.strip i { flex: 1; border-right: 1px solid var(--b-ground); }
.meta { position: absolute; left: 20mm; bottom: 46mm; margin: 0; font-size: 8pt; color: var(--b-muted); }
.intro { margin-bottom: 8mm; }
.contents-title { margin: 0 0 14mm; font: 700 24pt/1.1 var(--b-display); }
.contents { list-style: none; margin: 0; padding: 0; border-top: 2px solid var(--b-ink); }
.contents li { display: flex; gap: 10mm; padding: 4mm 0; border-bottom: 1px solid var(--b-line); font-size: 14pt; }
.no { font-variant-numeric: tabular-nums; color: var(--b-accent); font-weight: 700; }
.opener { display: flex; align-items: baseline; gap: 8mm; padding-bottom: 6mm; margin-bottom: 10mm; border-bottom: 2px solid var(--b-ink); }
.opener .no { font: 700 32pt/1 var(--b-display); }
.opener h2 { margin: 0; font: 700 28pt/1.05 var(--b-display); letter-spacing: -.02em; }
h3 { margin: 10mm 0 5mm; font: 700 13pt/1.3 var(--b-display); text-transform: capitalize; }
.prose { max-width: 150mm; }
.prose h3 { text-transform: none; }
.prose li + li { margin-top: 2mm; }
code { font-family: var(--font-mono, ui-monospace, monospace); font-size: .9em; }
.swatches { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 6mm 5mm; }
.swatch { margin: 0; break-inside: avoid; }
.swatch i { display: block; height: 22mm; border: 1px solid var(--b-line); }
.swatch figcaption { display: flex; flex-direction: column; gap: .5mm; padding-top: 2mm; font-size: 8.5pt; }
.swatch span { color: var(--b-muted); }
.family { display: flex; align-items: baseline; gap: 6mm; margin: 0 0 6mm; font-size: 30pt; }
.family span, .specimen code { font: 400 8pt var(--b-face); color: var(--b-muted); }
.specimen { padding: 5mm 0; border-top: 1px solid var(--b-line); break-inside: avoid; }
.specimen .label { display: flex; justify-content: space-between; gap: 6mm; }
.specimen p:last-child { margin: 3mm 0 0; }
.scale { display: grid; grid-template-columns: 36mm auto 24mm 1fr; align-items: center; gap: 4mm; margin: 0; padding: 2mm 0; border-bottom: 1px solid var(--b-line); font-size: 9pt; }
.scale i { display: block; height: 4mm; background: var(--b-accent); }
.scale span { color: var(--b-muted); }
.tiles { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 6mm; margin-top: 10mm; }
.tile { margin: 0; }
.tile i { display: block; height: 22mm; background: var(--b-ground); border: 1px solid var(--b-ink); }
.tile figcaption { display: flex; flex-direction: column; font-size: 8.5pt; padding-top: 2mm; }
.component { padding: 6mm 0; border-top: 1px solid var(--b-line); break-inside: avoid; }
.component h3 { margin: 1mm 0 2mm; text-transform: none; }
.component p { margin: 0 0 4mm; max-width: 140mm; }
.component iframe { display: block; width: 100%; border: 1px solid var(--b-line); background: var(--b-ground); }
.appendix { margin: 0 0 8mm; font: 700 18pt var(--b-display); }
.small { font-size: 9pt; color: var(--b-muted); }
@media print { html { background: none; } body { padding: 0; gap: 0; } }
`;
