# Artifact formats

What makes an HTML page a document, slide, page, whiteboard or sheet, and
a folder a deck, design or design system, as the host and the viewer parse it. How to
write them is in `skills/artifact/references/*.md`; how they are shown, in
[Artifacts](../architecture/artifacts.md).

## Page and folder kinds

`PageKind` reads an `.html` file's first 4 KiB, comments dropped and `'` as
`"`, for the literal `name="<name>"`; the first matching row wins.

| Meta tag | Kind | Content grammar |
| --- | --- | --- |
| `<meta name="sheet" content="1">` | `sheet` | in a page without tables, another value names an `.xlsx` or `.csv` beside it |
| `<meta name="print" content="flow …">` | `doc` | `flow`, then in any order a paper (`a3`, `a4`, `a5`, `letter`, `legal`, `<W>x<H>mm`; default `a4`), `landscape`, and margins: `normal` (25 mm), `narrow` (12.7), `moderate` (25/19), `wide` (25/50), or one to four lengths in `mm`, `cm`, `in`, `pt`, `px`, clockwise from the top |
| `<meta name="artboard" content="<W>x<H> slide">` | `slide` | `^(\d+)(?:x(\d+))?(\s+slide)?$` |
| `<meta name="artboard" content="<W>[x<H>]">` | `page` | a width alone lets the page scroll |

A folder with `tokens.css` is a `system`; one whose HTML pages, documents
and sheets aside, are all slides is a `deck`; any other with a page that
declares an artboard is a `design` (`FolderPages`). A document and a sheet
are each their own artifact wherever they lie. `<meta name="slide">`
holds words: `skip` leaves a slide out of presenting and the PDF, `push`
pushes it in when presented (others fade in). `<meta name="section"
content="Name">` names the part of a deck a slide opens (the legend shows
it). An element with `data-step="N"` (1–9) shows from the Nth click while
its slide is presented; every step shows otherwise. A slide's last
`body > aside` is its speaker notes. `<script type="application/json"
id="tweaks">` maps names to specs whose `default` is applied at load: `--x`
sets a CSS variable, `[a-z][a-z0-9-]{0,31}` a `data-` attribute on `<html>`.

In a document `<ul class="checklist">` holds items ticked by
`data-done` on the `<li>`; Files draws the boxes and writes the tick.

A page with `<meta name="board">` (and an artboard) is a whiteboard: each
`body` child with `data-at="X Y"` is an item at that place, sized by an
optional `data-size="W H"`; `class="note"` with `data-fill` (`yellow`,
`orange`, `pink`, `green`, `blue`, `grey`), `section`, `h1`/`h2`/`p`/`small`
text, `data-shape` (`rect`, `rounded`, `pill`, `ellipse`, `diamond`,
`hexagon`, `parallelogram`, `database`, `queue`, `document`, `cloud`,
`person`), `table`, `img`, `a` (a card), `class="widget"`. `<hr
data-from="id" data-to="id">` is an arrow, with `data-head` (`end`,
`both`, `none`), `data-dash` and `data-label`. An item needs an `id` to be
moved or joined.

On any page an element with `data-source` (a CSV with a header, or JSON
holding an array of objects, relative to the page), `data-field` (a column
or key), `data-where` (`col=value`, comma-separated, all must match; the
first row otherwise) and `data-format` (`#,##0`, `#,##0.00`, `0%`, `0.0%`,
`$#,##0`, `€#,##0.00`) is filled with that value as the page loads.

A deck's order is its sorted file names; reordering renames slides to
`NN-<name>.html`. A design puts pages starting with the same `NN-` in a row.

## Sheets

Tabs are the `<table data-sheet="Name">` elements in order; rows of only
`<th>` at the top are the header; the first row is 1, the first cell A.

| On | Attribute | Grammar |
| --- | --- | --- |
| table | `data-frozen` | `<rows> <letters>`, e.g. `1 A`; default the header |
| table | `data-merge` | ranges `A1:D1`, space-separated; not one cell, not overlapping, no text under the first cell |
| table | `data-group`, `data-folded` | row `2:12` and column `B:D` ranges; nest at most 7 deep, neither touch nor cut, not ending on the grid's last line; `data-folded` names groups |
| table, `<col>` | `data-rule` | `condition: effects`, `;`-separated; on the table the condition starts with a column letter and styles the row |
| `<col>`, cell | `data-format` | a number or date pattern, e.g. `#,##0.00` |
| `<col>` | `data-width`, `data-control="checkbox"`, `data-options="a\|b:good"` | pixels; TRUE/FALSE boxes; a dropdown, an option with `:fill` filling its cells |
| cell, `<tr>` | `data-fill` | `accent`, `good`, `warn`, `bad`, `muted` |
| cell, `<tr>` | `data-border`; `class="small\|large"` | `all`, `top`, `bottom`, `left`, `right`, space-separated; a text size |
| `<tr>` | `data-height` | pixels |
| cell | `data-ink`; `class` | `accent`, `good`, `warn`, `bad`, `muted`; `bold`, `italic`, `wrap` (also on `<col>`), `left`, `center`, `right` |
| `<tr>` | `class="total"` | bold, kept in place by a sort |
| `<figure>` after the table | `data-chart`, `data-range`, `data-at`, `data-size` | `bar`, `stacked`, `line`, `pie`; a box whose first row names series and first column labels; top-left cell; `<W>x<H>` cells (default `6x12`); `<figcaption>` is the title |

Rule conditions: `= v`, `<> v`, `< n`, `<= n`, `> n`, `>= n`, `blank`,
`not blank`, `before today`, `after today`; `v` is text, a number, a
percentage, `true`, `false` or `YYYY-MM-DD`. Effects: a fill, `bold`, `strike`.

## Errors

No file is refused for its grammar. What the viewer cannot read (print
words, merges, groups, rules, charts, markup in cells) and layout faults
are reported to Files and to the agent's `browser` `open`.

## Compatibility

No version field: the grammar is what this build parses; unknown tags and
attributes are ignored.

## Source and tests

| Path | What |
| --- | --- |
| `src/app/workspace_files.cc`, `src/app/present.cc` | `PageKind`, `ArtboardSize`, `FolderPages`, `DeckPage`; `present` |
| `web/public/design-helper.js`, `web/src/features/files/boards.ts`, `web/sheet/grid.js`, `web/sheet/book.js`, `web/sheet/rules.js`, `web/sheet/text.js` | print, artboard, notes, tweaks, builds, checklists, data marks, slide order; sheet grammar |
| `web/sheet/whiteboard.js`, `web/src/features/files/board-edit.ts` | whiteboard grammar and its edits |
| `web/tests/boards.test.js`, `web/tests/sheet-text.test.js`, `web/tests/sheet-rules.test.js`, `web/tests/sheet-groups.spec.js`, `web/tests/sheet-merge.spec.js`, `web/tests/sheet-format.spec.js`, `web/tests/board-edit.test.js`, `web/tests/whiteboard.spec.js`, `web/tests/data-marks.spec.js` | boards, sheet grammar, whiteboards, data marks |
