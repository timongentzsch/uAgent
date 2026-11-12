# A document

A document is one HTML file, `docs/q3-board-memo.html`, laid out on paper.
Files shows it a sheet of paper at a time, beside a legend of its pages;
people edit its text in place and export it to PDF and Markdown.

## The file

- Head: `<meta name="print" content="flow a4">`. A4 unless the person asks
  for another paper: `letter`, `legal`, `a3`, `a5`, `landscape` after the
  paper (`flow a4 landscape`), or a size in millimetres (`flow 152x229mm`).
- Margins come after the paper: `normal` (25 mm all round, the default),
  `narrow` (12.7 mm), `moderate` (25 mm top and bottom, 19 mm at the sides),
  `wide` (25 mm top and bottom, 50 mm at the sides), or lengths as in CSS
  (`flow a4 20mm 18mm`). The paper and its margins are set from this line,
  on screen and in the PDF: write no `@page` size or margin and no padding
  on the body.
- Keep the document's own styles in one `<style>` in the head. A page
  number goes in a margin box:
  `@page { @bottom-right { content: counter(page) " / " counter(pages); } }`.
- The body is the text, in reading order, as plain semantic HTML: `h1` (the
  title, once), `h2`, `h3`, `p`, `ul`/`ol`, `table` with `th`, `figure` with
  an image or inline SVG and a `figcaption`, `blockquote`. Write each block's
  text directly in its element, with only `b`, `i`, `a` and `code` inside:
  people edit text in place, block by block, mark it bold or italic, link
  it, and turn a block into a paragraph, an `h2`, an `h3` or a list item.
- No scripts, no absolute positioning, no fixed heights: the text flows from
  page to page, and each page is shown as its own sheet.

## Pages

- `h2, h3 { break-after: avoid; }`; `figure, table, blockquote
  { break-inside: avoid; }`; a chapter that must start on a new page gets
  `break-before: page`.
- A title page is optional. One that is used is a
  `<section class="title-page">`: it fills its page by itself (give it no
  height), so spread its title, summary and key figures over the page. A short document starts
  on page one instead.
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
