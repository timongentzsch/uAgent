// What can be done to the selection, as one list: a press on a cell, on a
// row's number, on a column's letter or on a header's mark shows the part
// of it that applies there.

import { headOf } from "./book.js";
import { fromTsv } from "./text.js";

const count = (n, one) => (n === 1 ? one : `${n} ${one}s`);
const HELD = /Mac|iP/.test(navigator.platform) ? "⌘" : "Ctrl+";
// The looks a cell may take (skills/artifact/references/sheet.md), as the
// app's format row names them.
const COLOURS = {
  accent: "Accent",
  good: "Good",
  warn: "Warning",
  bad: "Bad",
  muted: "Muted",
};
const FORMATS = {
  general: "General",
  "#,##0.00": "Number",
  "$#,##0.00": "Currency",
  "0%": "Percent",
  "yyyy-mm-dd": "Date",
};

// `clip` is the page's clipboard: what was copied here, and its commands.
export const actionsOf = (sheet, clip) => (on) => {
  const { book } = sheet;
  const at = sheet.pick().anchor;
  const { rows, columns } = sheet.tab();
  const inTable = at.row <= rows.length && at.column <= columns.length;
  const head = headOf(sheet.tab());
  const cell = on === "cell";
  // A header's own cell sorts and filters its column, as its mark does.
  const heads =
    on === "head" || on === "column" || (cell && inTable && at.row === head);
  const row = cell || on === "row";
  const column = cell || on === "column";
  const any = on !== "head";
  const run = (label, name, value) => ({
    label,
    run: () => sheet.run(name, value),
  });
  const shown = book.shown(sheet.at, at.row, at.column);
  const words = sheet.filterOf(at.column);
  const only = (label, sign) => ({
    label: `${label} “${shown}”`,
    run: () => sheet.filter(at.column, `${sign}${shown}`),
  });
  const span = (one, many, before, after) => [
    run(`Insert ${count(many, one)} ${before}`, `${one}-${before}`),
    run(`Insert ${count(many, one)} ${after}`, `${one}-${after}`),
    run(`Delete ${count(many, one)}`, `${one}-delete`),
  ];
  const looks = book.state(sheet.at, at.row, at.column);
  const one = (name, value, label, swatch) => ({
    ...run(label, name, value),
    checked: (looks[name] ?? "").toLowerCase() === value,
    swatch,
  });
  const colours = (name, none, from) => [
    one(name, "", none),
    ...Object.entries(COLOURS)
      .slice(from)
      .map(([value, label]) => one(name, value, label, `${name}-${value}`)),
  ];
  const held = sheet.held();
  // A file that is only read is copied from and filtered, nothing else.
  const changes = (items) => (sheet.locked ? [] : items);
  const groups = [
    any && [
      ...changes([{ label: "Cut", keys: `${HELD}X`, run: clip.cut }]),
      { label: "Copy", keys: `${HELD}C`, run: clip.copy },
      // Only what was copied here: the page cannot read the clipboard.
      ...changes(
        clip.copied()
          ? [
              { label: "Paste", keys: `${HELD}V`, run: clip.paste },
              {
                label: "Paste values only",
                run: () =>
                  sheet.did(
                    book.paste(
                      sheet.at,
                      { row: sheet.box().top, column: sheet.box().left },
                      fromTsv(clip.copied().text),
                    ),
                  ),
              },
            ]
          : [],
      ),
    ],
    [
      // One value of the column, shown alone or left out.
      ...(cell && inTable && at.row > head && shown
        ? [only("Show only", "="), only("Leave out", "<>")]
        : []),
      ...(heads
        ? [
            ...changes([
              run("Sort A to Z", "sort-up"),
              run("Sort Z to A", "sort-down"),
            ]),
            {
              field: "Filter: contains",
              value: words,
              change: (text) => sheet.filter(at.column, text),
            },
          ]
        : []),
      words && {
        label: "Clear filter",
        run: () => sheet.filter(at.column, ""),
      },
    ],
    changes([
      ...(row ? span("row", sheet.high(), "above", "below") : []),
      on === "row" && run("Move up", "row-move-up"),
      on === "row" && run("Move down", "row-move-down"),
      ...(column ? span("column", sheet.wide(), "left", "right") : []),
      on === "column" && run("Move left", "column-move-left"),
      on === "column" && run("Move right", "column-move-right"),
      on === "column" && run("Fit to content", "column-fit"),
    ]),
    any &&
      changes([
        run("Clear contents", "clear"),
        run("Clear formatting", "clear-looks"),
        {
          label: "Formatting",
          items: [
            { ...run("Bold", "bold"), checked: !!looks.bold },
            { ...run("Italic", "italic"), checked: !!looks.italic },
            { ...run("Wrap text", "wrap"), checked: !!looks.wrap },
            {
              label: "Align",
              items: ["left", "center", "right"].map((way) =>
                one("align", way, way[0].toUpperCase() + way.slice(1)),
              ),
            },
            { label: "Text colour", items: colours("ink", "Default", 1) },
            { label: "Fill", items: colours("fill", "None", 0) },
            {
              label: "Number format",
              items: Object.entries(FORMATS).map(([pattern, label]) =>
                one("format", pattern, label),
              ),
            },
          ],
        },
        // The app's dialog, on the active cell's column.
        column && {
          label: "Conditional formatting…",
          run: () => sheet.say({ type: "rules" }),
        },
      ]),
    changes([
      row && inTable && run("Freeze rows up to here", "freeze-rows"),
      column && inTable && run("Freeze columns up to here", "freeze-columns"),
      any && held.rows + held.columns > 0 && run("Unfreeze", "unfreeze"),
    ]),
    [
      ...changes([
        cell && run("Insert chart", "chart"),
        cell && {
          field: "Link to an address",
          value: "",
          change: (address) =>
            address.trim() &&
            sheet.write(
              `=HYPERLINK("${address.trim().replaceAll('"', '""')}","${(shown || address.trim()).replaceAll('"', '""')}")`,
            ),
        },
      ]),
      any &&
        sheet.offers("comment") && {
          label: "Comment",
          run: () => sheet.say({ type: "comment" }),
        },
    ],
  ];
  // The groups that hold something, a line between them.
  return groups
    .map((group) => (group || []).filter(Boolean))
    .filter((group) => group.length)
    .flatMap((group, n) => (n ? ["-", ...group] : group));
};
