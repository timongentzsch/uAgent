// Values a page takes from a CSV or JSON file of its folder, by its marks
// (skills/artifact/references/data.md), loaded by design-helper.js when a
// page has any. A pointer on a value says where it came from, and in the app
// opens the file at its row.
import { fromCsv as readCsv } from "./text.js";

const NUMBER = {
  "#,##0": { maximumFractionDigits: 0 },
  "#,##0.00": { minimumFractionDigits: 2, maximumFractionDigits: 2 },
  "0%": { style: "percent", maximumFractionDigits: 0 },
  "0.0%": {
    style: "percent",
    minimumFractionDigits: 1,
    maximumFractionDigits: 1,
  },
  "$#,##0": { style: "currency", currency: "USD", maximumFractionDigits: 0 },
  "€#,##0.00": { style: "currency", currency: "EUR" },
};
// A CSV file's rows as objects by its header, read as a sheet reads the
// file (sheet/text.js).
const fromCsv = (text) => {
  const [head, ...body] = readCsv(text);
  return body.map((row) =>
    Object.fromEntries(head.map((name, at) => [name.trim(), row[at] ?? ""])),
  );
};
const data = new Map();
const rowsOf = (source) => {
  if (!data.has(source))
    data.set(
      source,
      fetch(new URL(source, location.href))
        // A file that is not there answers without the header that lets
        // the page read it: the fetch itself fails.
        .catch(() => ({ ok: false }))
        .then(async (answer) => {
          if (!answer.ok) throw new Error(`${source} did not load`);
          const text = await answer.text();
          const rows = /\.json$/i.test(source)
            ? JSON.parse(text)
            : await fromCsv(text);
          if (!Array.isArray(rows)) throw new Error(`${source} holds no rows`);
          return rows;
        }),
    );
  return data.get(source);
};
const fillOne = async (mark) => {
  const { source, field, where = "", format } = mark.dataset;
  let rows;
  try {
    rows = await rowsOf(source);
  } catch (error) {
    return error instanceof SyntaxError
      ? `${source} is not JSON`
      : error.message;
  }
  if (rows.length && !Object.hasOwn(rows[0], field))
    return `${source} has no column "${field}"`;
  const wanted = where
    .split(",")
    .filter((part) => part.trim())
    .map((part) => part.split("=").map((side) => side.trim()));
  const at = rows.findIndex((row) =>
    wanted.every(([name, value]) => String(row[name]) === value),
  );
  if (at < 0) return `${source} has no row where ${where}`;
  const value = rows[at][field];
  const number = Number(value);
  mark.textContent =
    format && NUMBER[format] && value !== "" && Number.isFinite(number)
      ? new Intl.NumberFormat("en-US", NUMBER[format]).format(number)
      : String(value);
  mark.dataset.uagentRow = `${at + 1}`;
  if (format && !NUMBER[format])
    return `data-format "${format}" is not one a mark takes`;
};
// Where a value came from, while the pointer is on it: in the app, said
// for the app to draw at the pane's size (with Open, at the file's row);
// alone, a chip over it in the page.
let chip;
const watch = (base, send) => {
  addEventListener("pointerover", (event) => {
    const mark = event.target.closest?.("[data-uagent-row]");
    if (!mark) return;
    const { source, field, uagentRow } = mark.dataset;
    const label = `${source} · ${field} · row ${uagentRow}`;
    const { x, y, width, height } = mark.getBoundingClientRect();
    if (parent !== window) {
      const path = decodeURIComponent(
        new URL(source, location.href).pathname.slice(base.length),
      );
      // A CSV's row is the line after its header's; a JSON file opens whole.
      const line = /\.csv$/i.test(source) ? +uagentRow + 1 : 0;
      return send("source", {
        rect: { x, y, width, height },
        label,
        path,
        line,
      });
    }
    chip?.remove();
    chip = document.createElement("span");
    chip.textContent = label;
    Object.assign(chip.style, {
      position: "fixed",
      left: `${Math.max(0, x)}px`,
      top: `${y + height + 4}px`,
      zIndex: 2147483647,
      font: "12px/1.4 system-ui, sans-serif",
      background: "#111",
      color: "#fff",
      padding: "4px 8px",
    });
    document.body.append(chip);
  });
  addEventListener("pointerout", (event) => {
    if (!event.target.closest?.("[data-uagent-row]")) return;
    if (parent !== window) send("source", {});
    chip?.remove();
  });
};

// Fills the marks and watches the pointer on them: what was not filled is
// said, for the agent.
export async function fill(marks, base, send) {
  watch(base, send);
  return (await Promise.all(marks.map(fillOne))).filter(Boolean);
}
