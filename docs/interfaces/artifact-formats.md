# Artifact formats

What makes an HTML page a document, slide, page or sheet, and a folder a
deck, design or design system, as the host and the viewer parse it. How to
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
are each their own artifact wherever they lie. `<meta name="slide"
content="skip">` leaves a slide out of presenting and the PDF; a slide's
last `body > aside` is its speaker notes. `<script type="application/json"
id="tweaks">` maps names to specs whose `default` is applied at load: `--x`
sets a CSS variable, `[a-z][a-z0-9-]{0,31}` a `data-` attribute on `<html>`.

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
| `<col>` | `data-width`, `data-control="checkbox"`, `data-options="a\|b"` | pixels; TRUE/FALSE boxes; a dropdown |
| cell, `<tr>` | `data-fill` | `accent`, `good`, `warn`, `bad`, `muted` |
| cell | `data-ink`; `class` | `good`, `warn`, `bad`, `muted`; `bold`, `italic`, `wrap` (also on `<col>`), `left`, `center`, `right` |
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
| `web/public/design-helper.js`, `web/src/features/files/boards.ts`, `web/sheet/grid.js`, `web/sheet/book.js`, `web/sheet/rules.js`, `web/sheet/text.js` | print, artboard, notes, tweaks, slide order; sheet grammar |
| `web/tests/boards.test.js`, `web/tests/sheet-text.test.js`, `web/tests/sheet-rules.test.js`, `web/tests/sheet-groups.spec.js`, `web/tests/sheet-merge.spec.js` | boards, sheet grammar |
