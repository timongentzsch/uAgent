---
name: design
description: Design or restyle pages, screens, slide decks, documents (memos, reports) and a design system: HTML shown in Files
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

- One folder is one canvas: a flow or a product surface, e.g.
  `design/landing/` or `design/onboarding/` (in a workspace, any folder).
- Name pages `NN-screen[--state].html`, lower-case kebab-case:
  - `NN` is the step, two digits; it is the page's row on the canvas, so the
    variants and states of a step share it and sit side by side.
  - `screen` says what the page is; a `--state` (`--error`, `--empty`,
    `--annual`) marks a state or variant of it.
  - `design/onboarding/`: `01-welcome.html`, `02-sign-in.html`,
    `02-sign-in--error.html`, `03-workspace--empty.html`. Two directions of
    one screen: `01-hero-numbers.html`, `01-hero-session.html`.
  - The readable name is the page's `<title>` ("Sign in: wrong password"),
    which the canvas shows; never put words for people in the file name.
  - A page without a step (`book.html`) sits in the first row.
- Pages of one design may share a stylesheet in the folder (`style.css`); a
  change to it reloads every page.
- Each file is a complete HTML document:
  - `<title>` names the artboard.
  - `<meta name="artboard" content="1440x900">` fixes its size: a phone
    screen 390x844, a desktop screen 1440x900, a square post 1080x1080. A
    slide is `1920x1080 slide` (`references/decks.md`). `content="1440"` (a width
    only) makes a page: fluid, its height its content, laid out with
    `max-width`, `%`, `fr` and `rem` so it also works at a phone's width.
  - `<meta name="print" content="flow a4">` makes a document on paper that
    paginates when printed (`references/docs.md`); a fixed artboard prints as
    one page.
- Markup first. Draw every screen, card and row in HTML; scripts only for
  behaviour (state, a toggle, a carousel), never to build the page.
- Links between files (`<a href="02-sign-in.html">`) make a clickable
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
{ "--accent":  { "editor": "color", "default": "#2f6fde", "options": ["#e8590c", "#2f6fde", "#111111"] },
  "--density": { "editor": "range", "default": 1, "min": 0.8, "max": 1.3, "step": 0.05, "unit": "×" },
  "variant":   { "editor": "enum", "default": "split", "options": ["split", "stacked"] },
  "phone":     { "editor": "boolean", "default": true } }
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

- In your reply, name the folder (`design/landing/`), which opens the
  canvas, and a single page by its path from the folder you work in
  (`design/landing/01-hero-numbers.html`), never a bare file name.
- Check what you made before you reply, where the `browser` tool is
  available:
  1. After its last change, open every page you wrote or changed with
     `open` and its `path`. It loads as the canvas shows it and lists the
     page's problems: larger than its artboard, a document page that runs
     past its height, a file that did not load.
  2. Fix every problem and open the page again, until none is listed.
  3. Look at the page once and fix what reads wrong or strays from the
     design system's tokens. Pages you did not touch need no second look.
- Do not hand designs over with `artifact`: it snapshots a single file.
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
- Without a brand: type from the subject; if neutral, one sans for text and
  one for headings, from Google Fonts with a system fallback. A tinted ground,
  0–2 accents of like lightness and chroma. Commit to one nameable look.
- Avoid the tropes: gradient washes, emoji, cards with a coloured left border,
  everything centred and rounded.
- Accessible as drawn (WCAG AA): real controls with labels, 44px touch
  targets on phone screens.
- Do not recreate another company's distinctive design or branding.

## A design system

A design system is a folder, e.g. `design/system/`: `tokens.css` (colours
per theme, space, radius, shadow, fonts, text styles), `components/` (a
`bundle.css`, and per component a README and a `preview.html`), a
`README.md` of usage rules and `book.html`, its brand book. Pages use it with
`<link rel="stylesheet" href="../system/tokens.css">` (and `bundle.css`) and
`data-theme` on `<html>`. To build, re-sync or extend one, read
`references/design-system.md` first.

## Decks and documents

A slide deck (a presentation, a talk): read `references/decks.md` first. A
document on paper (a memo, report, brief, letter, handout): read
`references/docs.md` first. Both are pages of this kind, on the canvas, with
the design system.
