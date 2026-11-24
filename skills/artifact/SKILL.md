---
name: artifact
description: Open before making a spreadsheet, memo, report, deck, design or whiteboard
---

# Artifacts

An artifact is plain HTML in the folder, shown live and sandboxed in the
web's Files pane, beside the chat. There are six types. Read the reference
of the one you make before you write a file:

| Type | It is | Where | Read first |
| --- | --- | --- | --- |
| Document | a memo, report, brief, letter or handout on paper: one file | `docs/<name>.html` | `references/document.md` |
| Deck | a presentation or talk: a folder, one file per slide | `decks/<name>/` | `references/deck.md` |
| Design | screens, pages, posts or a flow on a canvas: a folder of pages | `design/<name>/` | `references/design.md` |
| Design system | tokens, components and a brand book the others link | `design/system/` | `references/design-system.md` |
| Sheet | a budget, tracker, table or model with formulas: one file | `sheets/<name>.html` | `references/sheet.md` |
| Whiteboard | a flow, map, retro or plan of notes, shapes and arrows | `boards/<name>/board.html`, or a page of a design | `references/whiteboard.md` |

In a workspace any folder serves. A new artifact is a new file or folder:
change an existing one only when the person asks to change it.

The person steps through a document's pages or a deck's slides, moves over a
design's canvas, reads a sheet as a calculated grid, points at an element
and comments, edits a document's or a slide's text in place, presents and exports. Their comments come back to you as lines in
their message. You make every change by editing the files.

## Before you build

- Read what exists first: the codebase's styles and components, screenshots,
  a design system folder. Work rooted in the real product beats work
  invented from memory; recreate from source, copying exact values, never
  rounding them.
- When the brief leaves two or more result-changing decisions open, ask once
  with `ask`: at most 4 questions, each with 2–4 concrete options for this
  piece (your pick first, marked "(Recommended)"), written from their
  material. A full brief or a small revision gets no questions: decide, and
  state your assumptions in one line.

## Every file

- A complete HTML document whose `<title>` is its readable name; file names
  are lower-case kebab-case and never carry words for people.
- When the folder has a design system, link its `tokens.css` (and
  `components/bundle.css`), set `data-theme` on `<html>`, and take every
  colour, size and face from its tokens.
- No network: a page loads nothing from another host, a font service
  included. Images, fonts and libraries are files in the folder, linked
  relatively.
  To use an image the person attached, copy it into the folder with
  `write_file` and `from`. Icons are inline stroke SVG; no emoji.
- Charts and diagrams are inline SVG drawn in the markup, labelled with
  their values.
- Numbers that live in a CSV or JSON file in the folder are marked, not
  typed: read `references/data.md`.

## Craft

- No filler: no lorem ipsum, invented numbers or quotes, or padding
  sections. A missing fact is a visible placeholder such as [YOUR PRICE]
  that you list in your reply. Ask before adding sections or copy nobody
  asked for.
- Without a brand: type from the subject, as a stack of faces the reader's
  system has (`system-ui, "Segoe UI", sans-serif`; `"Iowan Old Style",
  Palatino, Georgia, serif`; `ui-monospace, Menlo, Consolas, monospace`), or
  a font file the person put in the folder. If neutral, one stack for text
  and one for headings. A tinted ground, 0–2 accents of like lightness and
  chroma. Commit to one nameable look.
- Avoid the tropes: gradient washes, emoji, cards with a coloured left
  border, everything centred and rounded.
- Do not recreate another company's distinctive design or branding.

## Check, then present

- For a deck, use the overview and targeted review in `references/deck.md`.
- For other artifacts, use `browser open` with the changed page's `path` for
  layout and missing-file diagnostics. Use `observe` with its path to read
  the result at its intended size. Fix and recheck affected pages.
- Hand over the finished artifact with `present`: the folder for a deck,
  design or design system, the file for a document or sheet. Files exports
  PDFs and images.

In your reply, name a folder or page by its path from the folder you work
in (`design/landing/01-hero.html`), never a bare file name.

## Lines from the viewer

Their message may carry lines Files wrote:

- `` `path` `selector` “text” — comment ``: change that element, found by
  its text or selector, as the comment asks.
- `` `path` tweaks: --accent #2f6fde, … ``: write those values as defaults.
- `` `path` threw: message (line n) ``: fix the error.
- `` `system/tokens.css` theme dark: --page #0f1115, … ``: write those
  values in that theme's block of the design system.

Change only what was asked; suggest anything broader in your reply.
