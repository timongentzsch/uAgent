# Numbers from a data file

Any page (a design page, a slide, a document) can show values read from a
CSV or JSON file in its folder instead of typing them. Files fills each mark
when the page opens and again when the data file changes, and the person
can point at a value to see the file, column and row it came from.

```html
<p>Revenue in March:
  <b data-source="sales.csv" data-field="revenue" data-where="month=2027-03" data-format="$#,##0"></b></p>
```

- `data-source` is the file's path from the page, as a link's would be:
  `sales.csv` beside the page, `../../sales.csv` for one two folders up.
  A CSV with a header row, or JSON holding an array of objects.
- `data-field` is a column of the CSV (its header text, exactly) or a key
  of the objects.
- `data-where="column=value"` picks the row; several are joined with `,`
  (`region=EU, month=2027-03`), and all must match. Left out, it is the
  first row. When several rows match, the first one is shown.
- `data-format` shows a number as a sheet's patterns do: `#,##0`,
  `#,##0.00`, `0%`, `0.0%`, `$#,##0`, `€#,##0.00`. Left out, the value is
  shown as written in the file.
- Leave the element empty: Files writes the value into it. Check with
  `browser` `open`: a mark naming a missing file, column or row is listed.
- A total, share or change the file does not hold: add it to the file as a
  column or a row (a short script for many rows), then mark it. Do not
  compute numbers in the page.
