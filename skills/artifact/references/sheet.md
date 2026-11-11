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
  and how it shows.
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
| `data-control="checkbox"` on a `<col>` | TRUE and FALSE as boxes to tick |
| `data-options="To do\|In progress\|Done"` on a `<col>` | a dropdown |
| `data-frozen="1 A"` on the table | rows and columns that stay in view |

Nothing else: no merged cells, no fonts or borders, no `style`.

## Laying it out

- The pane shows about six columns: the important ones come first.
- A summary goes in rows above its table or on a tab of its own, never
  beside the table.
- Long text gets `class="wrap"` and a width of 240 to 400.
- One table per tab, its header in row 1 unless a summary sits above it.
- Many rows from a file: write a script that turns the file into the table,
  do not type the rows.
