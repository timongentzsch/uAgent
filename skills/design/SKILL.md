---
name: design
description: Design pages, screens, decks or a design system as HTML shown live in Files
---

# Design

A design is a folder of plain HTML files. The web's Files pane shows such a
folder as a canvas: every file is a live, sandboxed artboard, beside the chat.
The person opens one, points at an element and comments, tries the levers you
declared, presents or prints it. Their comments come back to you as lines in
their message. You make every change by editing the files.

## Before you build

- Read what exists first: the codebase's styles and components, screenshots,
  a design system folder (below). A hi-fi design rooted in the real product
  beats one invented from memory; recreate from source, copying exact values,
  never rounding them.
- When the brief leaves two or more result-changing decisions open, ask once
  with `ask`: at most 4 questions, each with 2–4 concrete options for this
  piece (your pick first, marked "(Recommended)"), written from their material.
  A full brief or a small revision gets no questions: decide, and state your
  assumptions in one line.
- No look given and nothing to match: offer 2–4 genuinely different directions
  as separate files in one row, each exploring an axis you can name, and say
  what each is for in your reply. Keep option names stable across turns.

## A design folder

- One folder is one canvas, e.g. `design/landing/` (in a workspace, any folder).
  Files named `<row>-<name>.html` form a row per `<row>` (`hero-numbers.html`,
  `hero-plain.html` → row "hero"); files without a `-` form the first row.
  Rows and files sort by name; to order them otherwise, start the row with a
  number (`1onboard-welcome.html`, `2today-home.html`), which the canvas does
  not show. Lower-case names, `-` only after the row.
- Pages of one design may share a stylesheet in the folder (`style.css`); a
  change to it reloads every page.
- Each file is a complete HTML document:
  - `<title>` names the artboard.
  - `<meta name="artboard" content="1440x900">` fixes its size: a phone
    screen 390x844, a desktop screen 1440x900, a slide 1920x1080, a square post
    1080x1080, a Letter page 816x1056, A4 794x1123. `content="1440"` (a width
    only) makes a page: fluid, its height its content, laid out with
    `max-width`, `%`, `fr` and `rem` so it also works at a phone's width.
  - `<meta name="print" content="flow letter">` (or `a4`) makes a document
    that paginates when printed; a fixed artboard prints as one page.
- Markup first. Draw every screen, card and row in HTML; scripts only for
  behaviour (state, a toggle, a carousel), never to build the page. Lay out
  groups with flex or grid and `gap`.
- Links between files (`<a href="hero-plain.html">`) make a clickable
  prototype; a flow that shares state is one file with sections it shows and
  hides.
- No network except Google Fonts (one `fonts.googleapis.com/css2` link).
  Images, fonts and libraries are files in the folder, linked relatively.
  To use an image the person attached, copy it into the folder with
  `write_file` and `from`. Icons are inline stroke SVG; no emoji.
- Never draw a fake status bar or keyboard on a phone screen.

## Tweaks: levers the person can try

Declare a few levers that cut across the design — an accent, a density, a
variant, a dark theme — in the page's head. Not copy, not a colour used once:

```html
<script type="application/json" id="tweaks">
{ "--accent":  { "editor": "color", "default": "#2f6fde", "options": ["#e8590c", "#2f6fde", "#111111"], "section": "Look" },
  "--density": { "editor": "range", "default": 1, "min": 0.8, "max": 1.3, "step": 0.05, "unit": "×", "section": "Look" },
  "variant":   { "editor": "enum", "default": "split", "options": ["split", "stacked"], "section": "Look" },
  "phone":     { "editor": "boolean", "default": true, "section": "Layout" } }
</script>
```

- Editors: `color` (3–4 `options` show as swatches), `range` (`int` and
  `float` too, with `min`, `max`, `step`, `unit`), `enum`, `boolean`.
- A name starting with `--` is a CSS variable on `:root`: define it in your
  CSS with the same default and use `var(--accent)`. Ranges set a bare
  number and `unit` is only shown, so give the unit in CSS:
  `gap: calc(var(--gap) * 1px)`. Any other name is a
  `data-` attribute on `<html>` (`data-variant="stacked"`, `data-phone="false"`)
  that your CSS selects on.
- `default` is the current value: when the person keeps a value, write it
  there and in the CSS.

## Working with the canvas

- Name the folder in your reply as `design/landing/`; the reference opens the
  canvas. Name a single artboard by its path.
- Do not render, screenshot or open the pages to check them unless asked: the
  canvas shows a page that fails, and the person will point. Do not hand
  designs over with `artifact`: it snapshots a single file.
- Their message may carry lines the canvas wrote:
  - `` `path` `selector` “text” — comment ``: change that element, found by its
    text or selector, as the comment asks.
  - `` `path` tweaks: --accent #2f6fde, … ``: write those values as defaults.
  - `` `path` threw: message (line n) ``: fix the error.
  - `` `system/tokens.css` theme dark: --page #0f1115, … ``: write those
    values in that theme's block of the design system.
  Change only what was asked; suggest anything broader in your reply.

## Craft

- No filler: no lorem ipsum, invented numbers or padding sections. A missing
  fact is a visible placeholder such as [YOUR PRICE]. Ask before adding
  sections or copy nobody asked for.
- Follow an existing design's vocabulary: its type, colours, spacing, density
  and states.
- Without a brand: type from the subject; if neutral, one sans for text and
  one for headings, from Google Fonts with a system fallback. A tinted ground,
  0–2 accents of like lightness and chroma. Commit to one nameable look.
- Avoid the tropes: gradient washes, emoji, cards with a coloured left border,
  everything centred and rounded.
- Accessible as drawn: real `<button>`, `<a href>`, `<input>` with `<label>`;
  `aria-label` on icon-only buttons; text contrast at least 4.5:1 (3:1 from
  24px); touch targets at least 44px on phone screens; colours that must be
  told apart differ in lightness too.
- Print: body text at least 12pt (16px), margins about 72px, a document's
  headings in order; keep fixed pages fully inside their size.
- Do not recreate another company's distinctive design or branding.

## A design system

A design system is a folder, e.g. `design/system/`, that pages link:

- `README.md`: the brand book. Usage rules that name tokens ("buttons are
  `--accent` on `--page`"), voice, iconography, the source it came from and
  its commit, and a "Not synced" list of what could not be placed.
- `tokens.css`: the only token file.
  - Colours in a `:root, [data-theme="light"] { … }` block and one
    `[data-theme="<id>"] { … }` block per further theme.
  - `--space-*`, `--radius-*`, `--shadow-*` and `--font-<family>` variables.
  - A `.text-<style>` class per text style (`font`, `letter-spacing`).
  - One `/* usage */` comment after each declaration.
- `components/bundle.css` and, per component, `components/<Name>/README.md`
  (first sentence: what it is for; then when to use it and its classes) and
  `components/<Name>/preview.html`, a small page showing it in a few states,
  whose first line is `<!-- @dsCard group="Actions" height=88 -->`.
- `fonts/` and `assets/<Group>/` (logos, icons): copied files, never redrawn.
- Pages use it with `<link rel="stylesheet" href="../system/tokens.css">` (and
  `bundle.css`) and `data-theme` on `<html>`.

Building one from a codebase:

1. Ask once what to include if it is unclear: tokens, fonts, logos and icons,
   docs, which components.
2. Take tokens exactly as the code has them: CSS custom properties under
   `:root` and theme selectors (`[data-theme="dark"]`, `.dark`, a
   `prefers-color-scheme: dark` block), a token file or a theme object.
   Keep names, values and aliases (`var(--other)`); skip what is computed
   (`calc()`, `color-mix()`) and list it under "Not synced". Never invent a
   value.
3. Copy font files, logos and icons; condense the docs into usage rules.
4. Components: list them, ask which, and write each as CSS classes with a
   preview, from the code's real styles.
5. On a later re-sync, update changed values, add new ones, keep notes and
   prose, and list tokens the code no longer defines; ask before removing them.
