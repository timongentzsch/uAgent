# A document

A document is one HTML file, `docs/q3-board-memo.html`, laid out on paper.
Files shows it beside a legend of its pages; people edit its text in place
and export it to PDF and Markdown.

## The file

- Head: `<meta name="print" content="flow a4">`. A4 unless the person asks
  for another paper: `letter`, `legal`, `a3`, `a5`, `landscape` after the
  paper (`flow a4 landscape`), or a size in millimetres (`flow 152x229mm`).
  It is shown as wide as its paper and needs no artboard.
- Keep the document's own styles in one `<style>` in the head.
- `@page { size: A4; margin: 20mm 18mm; }` (the paper you chose), with the
  page number in a margin box:
  `@page { @bottom-right { content: counter(page) " / " counter(pages); } }`.
- The body is the text, in reading order, as plain semantic HTML: `h1` (the
  title, once), `h2`, `h3`, `p`, `ul`/`ol`, `table` with `th`, `figure` with
  an image or inline SVG and a `figcaption`, `blockquote`. Write each block's
  text directly in its element, with only `b`, `i`, `a` and `code` inside:
  people edit text in place, block by block.
- No scripts, no absolute positioning, no fixed heights: the text flows from
  page to page. On screen the body is the paper's width with the page's
  margins as padding, so it reads the way it prints.

## Pages

- `h2, h3 { break-after: avoid; }`; `figure, table, blockquote
  { break-inside: avoid; }`; a chapter that must start on a new page gets
  `break-before: page`.
- A title page is optional. One that is used fills its page: a
  `<section class="title-page">` as tall as the page inside its margins
  (`height: calc(297mm - 40mm)` for A4 with 20mm margins), with
  `break-after: page`, its title, summary and key figures spread over that
  height. A short document starts on page one instead.
- A table longer than half a page repeats its header row (`thead`) and may
  break between rows; keep each row on one page
  (`tr { break-inside: avoid; }`).

## Writing and typesetting

- Body 10–12pt (13–16px), line height 1.4–1.6, a measure of 60–80
  characters; headings from the design system's text styles.
- Print-safe colour: text on white, tinted rules and table stripes, one
  accent. Backgrounds print, but only use them for small blocks.
- Charts have labelled axes; no chart scripts.
- Lead with the point. Short paragraphs; a table for any comparison of more
  than two things; a list only for parallel items.
- When you look at it, look for a heading alone at the foot of a page, a
  figure cut by a break and a table wider than the paper.
