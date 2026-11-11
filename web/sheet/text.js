// What a sheet works out without the page or the engine, so it is tested
// alone: names, tab-separated text, moved formulas, the file's table.

export const letters = (column) => {
  let name = "";
  for (let n = column; n > 0; n = Math.floor((n - 1) / 26))
    name = String.fromCharCode(65 + ((n - 1) % 26)) + name;
  return name;
};
const columnOf = (name) =>
  [...name.toUpperCase()].reduce(
    (n, letter) => n * 26 + letter.charCodeAt(0) - 64,
    0,
  );
export const cellName = ({ row, column }) => `${letters(column)}${row}`;

export const boxOf = (a, b) => ({
  top: Math.min(a.row, b.row),
  left: Math.min(a.column, b.column),
  bottom: Math.max(a.row, b.row),
  right: Math.max(a.column, b.column),
});
export const boxName = ({ top, left, bottom, right }) => {
  const first = cellName({ row: top, column: left });
  const last = cellName({ row: bottom, column: right });
  return first === last ? first : `${first}:${last}`;
};

// `data-frozen="2 B"` holds two rows and columns A and B in view; unsaid,
// the header rows and no column.
export const frozenOf = (said = "", head = 0) => ({
  rows: Number(/\d+/.exec(said)?.[0] ?? head),
  columns: columnOf(/[A-Za-z]+/.exec(said)?.[0] ?? ""),
});

// Ctrl and an arrow: along what is filled to its end, else to the next
// filled cell, else to the edge.
export function edgeFrom(filled, size, from, by) {
  const next = (cell) => ({
    row: cell.row + by.row,
    column: cell.column + by.column,
  });
  const inside = ({ row, column }) =>
    row >= 1 && column >= 1 && row <= size.rows && column <= size.columns;
  let at = from;
  if (!inside(next(at))) return at;
  const along = filled(at) && filled(next(at));
  do at = next(at);
  while (inside(next(at)) && (along ? filled(next(at)) : !filled(at)));
  return at;
}

// As other spreadsheets copy cells: one with a tab, a line break or a
// quote in it is quoted, its quotes doubled.
export const toTsv = (rows) =>
  rows
    .map((row) =>
      row
        .map((cell) =>
          /["\t\n\r]/.test(cell) ? `"${cell.replaceAll('"', '""')}"` : cell,
        )
        .join("\t"),
    )
    .join("\n");
export function fromTsv(text) {
  const rows = [[]];
  const cell = /"((?:[^"]|"")*)"(?=\t|\r?\n|$)|([^\t\r\n]*)/y;
  for (let at = 0; at <= text.length; ) {
    cell.lastIndex = at;
    const [all, quoted, plain] = cell.exec(text);
    rows.at(-1).push(quoted?.replaceAll('""', '"') ?? plain);
    at += all.length;
    if (text[at] === "\t") at++;
    else if (at < text.length) {
      at += text.startsWith("\r\n", at) ? 2 : 1;
      // A last line break ends the last row; it starts no empty one.
      if (at < text.length) rows.push([]);
      else break;
    } else break;
  }
  return rows;
}

// "$B2", "B2:C$4", "A:A".
const END = String.raw`(\$?)([A-Za-z]*)(\$?)(\d*)`;
const ADDRESS = new RegExp(`^(\\s*)${END}(?::${END})?$`);
const tabOf = (prefix) =>
  prefix
    .trim()
    .replace(/^'(.*)'$/, "$1")
    .replaceAll("''", "'");

// `spans` count characters, as the engine does. With `own` (a sorted row
// and its tab) only references to that row move. Null when a reference
// cannot be read or lies partly on that row: a guess would be written.
export function moved(formula, spans, by, own) {
  const chars = [...formula];
  let out = "";
  let from = 0;
  for (const { start, end } of spans) {
    const text = chars.slice(start, end).join("");
    const cut = text.lastIndexOf("!") + 1;
    const read = ADDRESS.exec(text.slice(cut));
    // "A1 : B2" comes as two references with the colon between them.
    const ranged =
      /:\s*$/.test(chars.slice(0, start).join("")) ||
      /^\s*:/.test(chars.slice(end).join(""));
    if (!read || (own && ranged)) return null;
    const ends = [read.slice(2, 6), read.slice(6)].filter(
      ([, name, , digits]) => name || digits,
    );
    const mine = ends.filter(([, , , digits]) => Number(digits) === own?.row);
    const here = !cut || tabOf(text.slice(0, cut - 1)) === own?.tab;
    if (own && here && mine.length && mine.length < ends.length) return null;
    out += chars.slice(from, start).join("");
    from = end;
    if (own && !(here && mine.length)) {
      out += text;
      continue;
    }
    const cells = ends.map(([fixedColumn, name, fixedRow, digits]) => {
      const column = columnOf(name) + (fixedColumn ? 0 : by.column);
      const row = Number(digits) + (fixedRow ? 0 : by.row);
      return (name && column < 1) || (digits && row < 1)
        ? null
        : `${fixedColumn}${name && letters(column)}${fixedRow}${digits && row}`;
    });
    out += cells.includes(null)
      ? "#REF!"
      : `${text.slice(0, cut)}${read[1]}${cells.join(":")}`;
  }
  return out + chars.slice(from).join("");
}

// Numbers, then text, then TRUE and FALSE, then errors; what is empty comes
// last whichever way the sort runs.
const KINDS = { number: 0, text: 1, boolean: 2, error: 3, empty: 4 };
export const inOrder = (a, b, descending) => {
  if (a.kind !== b.kind) {
    if (a.kind === "empty" || b.kind === "empty")
      return a.kind === "empty" ? 1 : -1;
    return (KINDS[a.kind] - KINDS[b.kind]) * (descending ? -1 : 1);
  }
  const order =
    a.kind === "number"
      ? a.value - b.value
      : String(a.value).localeCompare(String(b.value), undefined, {
          numeric: true,
          sensitivity: "base",
        });
  return descending ? -order : order;
};

// The form skills/artifact/references/sheet.md teaches: a row a line, no
// closing tags for rows and cells, every cell written.
const escaped = (text) =>
  text.replace(
    /[&<>]/g,
    (sign) => `&${{ "&": "amp", "<": "lt", ">": "gt" }[sign]};`,
  );
const attributes = (attrs) =>
  Object.entries(attrs)
    .map(([name, value]) =>
      value === ""
        ? ` ${name}`
        : ` ${name}="${escaped(value).replaceAll('"', "&quot;")}"`,
    )
    .join("");
export const tableText = ({ name, attrs, columns, rows }) =>
  [
    `<table${attributes({ "data-sheet": name, ...attrs })}>`,
    ...(columns.some((column) => Object.keys(column).length)
      ? [columns.map((column) => `<col${attributes(column)}>`).join("")]
      : []),
    ...rows.map(
      (row) =>
        `<tr${attributes(row.attrs)}>${row.cells
          .map(
            (cell) =>
              `<${cell.th ? "th" : "td"}${attributes(cell.attrs)}>${escaped(cell.text)}`,
          )
          .join("")}`,
    ),
    "</table>",
  ].join("\n");
