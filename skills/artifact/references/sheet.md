# A sheet

A sheet is one HTML file, `sheets/<name>.html`: a workbook of tables that
Files shows as a spreadsheet grid and calculates. You edit the file. Checked
with `browser` `open`, it lists every cell in error with the reason.

## The file

```html
<!doctype html>
<meta charset="utf-8">
<meta name="sheet" content="1">
<title>Budget 2027</title>
<table data-sheet="Costs" data-frozen="1">
<col data-width="180"><col data-format="#,##0.00"><col data-format="#,##0.00"><col data-format="0%">
<tr><th>Item<th>Monthly<th>Yearly<th>Share
<tr><td>Rent<td>1200<td>=B2*12<td>=B2/B$4
<tr><td>Power<td>90<td>=B3*12<td>=B3/B$4
<tr class="total"><td>Total<td>=SUM(B2:B3)<td>=SUM(C2:C3)<td>=SUM(D2:D3)
</table>
<table data-sheet="Inputs">
<tr><th>Name<th>Value
<tr><td>Increase<td>4.5%
</table>
```

- The `<meta name="sheet">` line makes the page a sheet. No scripts and no
  styles: the viewer draws the grid.
- Each `<table data-sheet="Name">` is a tab, in the order written. One `<tr>`
  per line, no closing tags for rows and cells.
- A cell holds what a person would type: a number, a date (`2027-03-01`),
  text, or a formula starting with `=`. `4.5%` and `$1,200` set the value
  and how it shows. As anywhere in HTML, `<` is written `&lt;` and `&` is
  `&amp;`: `=IF(B2&lt;C2,"under","over")`.
- Cells are named by their place in the table: the first `<tr>` is row 1
  (the header counts), the first cell is column A. Another tab is
  `Inputs!B2`. An empty cell is still written (`<td>`), so the columns line
  up.
- Formulas are Excel's, in English with commas: `SUM`, `SUMIFS`, `COUNTIF`,
  `AVERAGE`, `IF`, `IFERROR`, `XLOOKUP`, `INDEX`/`MATCH`, `ROUND`, `TEXT`,
  `DATE`, `EDATE`, `PMT`, `NPV`. Never write a number a formula can give:
  totals, shares and counts are formulas over ranges.

## Marks the viewer understands

| In the file | Shown as |
| --- | --- |
| `<th>` cells in the first rows | the header band, held in view, with sort and filter |
| `<tr class="total">` | a total row: bold, a line above, left in place by a sort |
| `data-format="#,##0.00"` on a `<col>` or a cell | a number or date pattern |
| `data-width="240"` on a `<col>` | the column's width in pixels |
| `class="wrap"` on a `<col>` or a cell | long text wraps |
| `data-fill="accent\|good\|warn\|bad\|muted"` on a cell or row | a fill from the theme |
| `data-ink="good\|warn\|bad\|muted"` on a cell | the text's colour, from the theme |
| `class="bold"`, `"italic"`, `"left"`, `"center"`, `"right"` on a cell | bold, italic, and where its text stands |
| `data-control="checkbox"` on a `<col>` | TRUE and FALSE as boxes to tick |
| `data-options="To do\|In progress\|Done"` on a `<col>` | a dropdown |
| `data-frozen="1 A"` on the table | rows and columns that stay in view |
| `data-merge="A1:D1 A5:A7"` on the table | each range as one cell showing its top-left cell; the others are written empty |
| `data-group="2:12 3:6 B:D"` on the table | rows 2 to 12 and 3 to 6, and columns B to D, as groups a reader folds; groups nest or leave a row between |
| `data-folded="3:6"` on the table | those groups shown folded |
| `data-rule="< 0: bad"` on a `<col>` | the column's cells that meet the condition take the effect |
| `data-rule="D = Done: strike muted"` on the table | the rows whose column D meets it take the effect |
| `<figure data-chart="bar" data-range="A1:C4">` after the table | a chart of those cells |

Nothing else: no fonts or borders, no `style`.

## Rules and charts

- A rule is `condition: effects`; several are separated by `;`. Conditions:
  `= text`, `<> text`, `< 0`, `<= 0`, `> 0`, `>= 0`, `blank`, `not blank`,
  `before today`, `after today`. A value is text, a number, a percentage
  or a date as `2027-03-01`. Effects: a fill (`accent`, `good`, `warn`,
  `bad`, `muted`), `bold`, `strike`. On the table the condition starts with
  a column's letter: `data-rule="D = Done: strike muted; C before today: bad"`.
  A later rule's fill lies over an earlier one's.
- A chart is written directly after the table it draws from:
  `<figure data-chart="bar" data-range="A1:C4" data-at="F2" data-size="6x12"><figcaption>Costs per item</figcaption></figure>`.
  `data-chart` is `bar`, `stacked`, `line` or `pie`. The range's first row
  names the series and its first column the labels; a pie takes the first
  series. `data-at` is the cell its top left corner sits on, beside or
  under the data, and `data-size` its width and height in cells of the
  usual size (96 by 24 pixels); left out, the chart sits beside the table.

## Laying it out

- The pane shows about six columns: the important ones come first.
- A summary goes in rows above its table or on a tab of its own, never
  beside the table.
- A title or a label over several columns is merged, not grouped; data cells
  are not: a sort cannot run through a merge.
- Detail rows that a subtotal sums are a group, the subtotal in the row after
  them.
- Long text gets `class="wrap"` and a width of 240 to 400.
- One table per tab, its header in row 1 unless a summary sits above it.
- Many rows from a file: write a script that turns the file into the table,
  do not type the rows.
