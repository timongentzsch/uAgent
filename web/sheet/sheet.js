// A sheet page drawn as a spreadsheet: every <table data-sheet> is a tab,
// a cell's text is what a person would type, and the engine beside this
// file calculates what each cell shows. The page's own markup stays the
// record; this only draws over it. The format: skills/artifact/references/
// sheet.md.

const tables = [...document.querySelectorAll("table[data-sheet]")];
const say = (message) =>
  parent.postMessage({ uagentDesign: 1, ...message }, "*");
const letters = (column) => {
  let name = "";
  for (let n = column; n > 0; n = Math.floor((n - 1) / 26))
    name = String.fromCharCode(65 + ((n - 1) % 26)) + name;
  return name;
};
const here = new URL(".", import.meta.url).href;
// The engine's cell types this needs: a number is set to the right, an
// error is reported.
const NUMBER = 1;
const ERROR = 16;

// The grid's look, for a live sheet and for a still one alike.
document.head.append(
  Object.assign(document.createElement("link"), {
    rel: "stylesheet",
    href: `${here}sheet.css`,
  }),
);

// What a table's <col>s say of each column: width, format, wrapping, a
// checkbox or a list of options.
const columnsOf = (table) =>
  [...table.querySelectorAll("col")].flatMap((col) =>
    Array.from({ length: col.span }, () => ({
      ...col.dataset,
      wrap: col.classList.contains("wrap"),
    })),
  );

// A header row: every cell written in it is a <th>. It need not be the
// first row: a summary may sit above its table.
const isHeader = (cells) =>
  cells.length > 0 && cells.every((cell) => cell.localName === "th");

// Row numbers down the side and column letters across the top, as cells of
// the table itself so they scroll, print and are read with it.
function frame(table, width) {
  const corner = (text, scope) =>
    Object.assign(document.createElement("th"), {
      className: "sheet-edge",
      textContent: text,
      scope,
    });
  [...table.rows].forEach((row, at) => row.prepend(corner(at + 1, "row")));
  const top = table.createTHead().insertRow(0);
  top.className = "sheet-letters";
  top.append(corner("", "col"));
  for (let column = 1; column <= width; column++)
    top.append(corner(letters(column), "col"));
}

async function draw() {
  const { default: init, Model } = await import(`${here}wasm.js`);
  await init();
  // Drawn in a task of its own: the engine can come ready inside the report
  // of a ResizeObserver (the page helper's), where a page that resizes
  // itself is in error.
  await new Promise((done) => setTimeout(done));
  const model = new Model(document.title || "Sheet", "en", "UTC", "en");
  const names = tables.map((table) => table.dataset.sheet);
  // Every cell as written, before anything is drawn over it.
  const written = tables.map((table) =>
    [...table.rows].map((row) => [...row.cells]),
  );
  model.pauseEvaluation();
  tables.forEach((table, sheet) => {
    if (sheet) model.newSheet();
    model.renameSheet(sheet, names[sheet]);
  });
  tables.forEach((table, sheet) => {
    const columns = columnsOf(table);
    const format = (cell, row, column) => {
      const pattern = cell.dataset.format ?? columns[column - 1]?.format;
      if (pattern)
        model.updateRangeStyle(
          { sheet, row, column, width: 1, height: 1 },
          "num_fmt",
          pattern,
        );
    };
    written[sheet].forEach((cells, r) =>
      cells.forEach((cell, c) => {
        cell.dataset.raw = cell.textContent.trim();
        if (cell.dataset.raw)
          model.setUserInput(sheet, r + 1, c + 1, cell.dataset.raw);
        format(cell, r + 1, c + 1);
      }),
    );
  });
  model.resumeEvaluation();
  model.evaluate();

  const errors = [];
  tables.forEach((table, sheet) => {
    const columns = columnsOf(table);
    const width = Math.max(...written[sheet].map((cells) => cells.length), 1);
    // Frozen as the table says, else through its last header row.
    const head = written[sheet].findLastIndex(isHeader) + 1;
    const frozenRows = Number(
      /\d+/.exec(table.dataset.frozen ?? "")?.[0] ?? head,
    );
    written[sheet].forEach((cells, r) => {
      const row = table.rows[r];
      row.classList.toggle("sheet-head", isHeader(cells));
      row.classList.toggle("sheet-frozen", r < frozenRows);
      cells.forEach((cell, c) => {
        const column = columns[c] ?? {};
        const shown = model.getFormattedCellValue(sheet, r + 1, c + 1);
        const ref = `${letters(c + 1)}${r + 1}`;
        cell.dataset.ref = ref;
        cell.classList.toggle("sheet-wrap", !!column.wrap);
        const type = model.getCellType(sheet, r + 1, c + 1);
        cell.classList.toggle("sheet-number", type === NUMBER);
        if (type === ERROR) {
          cell.classList.add("sheet-error");
          errors.push(`${names[sheet]}!${ref} ${cell.dataset.raw} is ${shown}`);
        }
        if (
          column.control === "checkbox" &&
          r >= head &&
          /^(TRUE|FALSE)$/i.test(shown)
        )
          cell.replaceChildren(
            Object.assign(document.createElement("input"), {
              type: "checkbox",
              checked: /^true$/i.test(shown),
              disabled: true,
              ariaLabel: ref,
            }),
          );
        // Text the author marked up (<b>, <a>) stays as written.
        else if (cell.dataset.raw.startsWith("=") || cell.children.length === 0)
          cell.textContent = shown;
      });
    });
    table.querySelectorAll("col").forEach((col) => {
      if (col.dataset.width) col.style.width = `${col.dataset.width}px`;
    });
    table.prepend(
      Object.assign(document.createElement("colgroup"), {
        innerHTML: '<col class="sheet-edge">',
      }),
    );
    frame(table, width);
    // Each frozen row holds under the ones above it.
    for (const row of table.querySelectorAll(".sheet-frozen"))
      row.style.setProperty("--sheet-top", `${row.offsetTop}px`);
  });

  // One tab at a time; the app draws the tabs and the formula bar.
  const show = (active) => {
    if (!tables[active]) return;
    tables.forEach((table, at) => (table.hidden = at !== active));
    say({ type: "sheet", tabs: names, active });
  };
  let chosen;
  const choose = (cell) => {
    chosen?.removeAttribute("aria-selected");
    chosen = cell;
    cell.setAttribute("aria-selected", "true");
    say({
      type: "cell",
      ref: `${cell.closest("table").dataset.sheet}!${cell.dataset.ref}`,
      raw: cell.dataset.raw,
      shown: cell.textContent,
    });
  };
  addEventListener("click", (event) => {
    const cell = event.target.closest?.("[data-ref]");
    if (cell) choose(cell);
  });
  addEventListener("message", (event) => {
    if (event.data?.uagentDesign !== 1 || event.data.type !== "tab") return;
    show(event.data.index);
  });
  show(0);
  return errors.slice(0, 20);
}

// Done once the sheet is drawn, with its cells in error: the page helper
// that loaded this reports them with the page's other problems.
export const ready = tables.length ? draw() : Promise.resolve([]);
