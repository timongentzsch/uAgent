// One field holds the focus, unseen on the active cell, so typing replaces
// and the clipboard works as in any field.

import { headOf } from "./book.js";
import { cellAt } from "./grid.js";
import { openMenu } from "./menu.js";
import { edgeFrom, fromTsv, toTsv } from "./text.js";

const ARROWS = {
  ArrowUp: { row: -1, column: 0 },
  ArrowDown: { row: 1, column: 0 },
  ArrowLeft: { row: 0, column: -1 },
  ArrowRight: { row: 0, column: 1 },
};
const count = (n, one) => (n === 1 ? one : `${n} ${one}s`);

export function listen(sheet) {
  const { book } = sheet;
  const editor = Object.assign(document.createElement("input"), {
    className: "sheet-editor",
    ariaLabel: "Cell",
    autocomplete: "off",
    spellcheck: false,
  });
  document.body.append(editor);
  let editing = false;
  // The open list of an options column.
  let list;
  // What was copied or cut here, known again when it is pasted.
  let copied;

  const active = () => sheet.pick().anchor;
  // Past the table a cell is blank, in a column that says nothing.
  const cell = ({ row, column } = active()) =>
    sheet.tab().rows[row - 1]?.cells[column - 1] ?? { text: "" };
  const columnOf = (at = active()) => sheet.tab().columns[at.column - 1] ?? {};
  const inBody = (at) => at.row > headOf(sheet.tab());
  // The table's last cell: the first, where it holds none.
  const last = () => ({
    row: sheet.tab().rows.length || 1,
    column: sheet.tab().columns.length || 1,
  });
  const hold = () => editor.focus({ preventScroll: true });
  const place = (field) => {
    const box = cellAt(sheet.table(), active())?.getBoundingClientRect();
    if (box)
      Object.assign(field.style, {
        left: `${box.left + scrollX}px`,
        top: `${box.top + scrollY}px`,
        width: `${box.width}px`,
        height: `${box.height}px`,
      });
  };

  const rest = () => {
    editing = false;
    const gone = list;
    list = undefined;
    gone?.remove();
    place(editor);
    delete editor.dataset.editing;
    // No keyboard comes up on a phone while the field only holds focus.
    editor.inputMode = "none";
    // Something is selected, so copy is offered and typing replaces it.
    editor.value = cell()?.text || " ";
    if (document.activeElement === editor) editor.select();
  };
  editor.addEventListener("focus", () => editing || editor.select());

  const choose = (options) => {
    rest();
    list = Object.assign(document.createElement("select"), {
      className: "sheet-editor",
      ariaLabel: "Options",
    });
    list.dataset.editing = "";
    for (const text of new Set(["", ...options, cell().text]))
      list.append(new Option(text, text));
    list.value = cell().text;
    place(list);
    document.body.append(list);
    const close = (text) => {
      rest();
      if (text !== undefined) sheet.write(text);
      hold();
    };
    list.addEventListener("change", () => close(list.value));
    list.addEventListener("keydown", (event) => {
      if (event.key === "Escape") close();
    });
    list.addEventListener("blur", () => list && rest());
    list.focus();
  };
  const edit = (typed) => {
    if (sheet.locked) return rest();
    const options = inBody(active()) && columnOf()["data-options"];
    if (options) return choose(options.split("|"));
    editing = true;
    editor.dataset.editing = "";
    editor.inputMode = "text";
    hold();
    if (!typed) {
      editor.value = cell().text;
      editor.setSelectionRange(editor.value.length, editor.value.length);
    }
  };
  const commit = (by) => {
    const text = editor.value;
    rest();
    sheet.write(text);
    if (by) sheet.select(stepped(active(), by));
  };
  editor.addEventListener("input", () => editing || edit(true));
  editor.addEventListener("blur", () => editing && commit());

  const stepped = (from, by) => ({
    row: from.row + by.row,
    column: from.column + by.column,
  });
  const toggle = () => {
    const ticked = /^(TRUE|FALSE)$/i.exec(cell().text)?.[1].toUpperCase();
    if (ticked) sheet.write(ticked === "TRUE" ? "FALSE" : "TRUE");
  };
  const isBox = (at = active()) =>
    inBody(at) && columnOf(at)["data-control"] === "checkbox";

  const command = (label, name) => ({ label, run: () => sheet.run(name) });
  const headItems = () => {
    const { column } = active();
    const words = sheet.filterOf(column);
    return [
      !sheet.locked && command("Sort A to Z", "sort-up"),
      !sheet.locked && command("Sort Z to A", "sort-down"),
      {
        field: "Filter: contains",
        value: words,
        change: (text) => sheet.filter(column, text),
      },
      words && { label: "Clear filter", run: () => sheet.filter(column, "") },
    ];
  };
  const spanItems = (one, many, before, after) => {
    const what = count(many, one);
    return [
      command(`Insert ${what} ${before}`, `${one}-${before}`),
      command(`Insert ${what} ${after}`, `${one}-${after}`),
      command(`Delete ${what}`, `${one}-delete`),
    ];
  };
  const rowItems = () => spanItems("row", sheet.high(), "above", "below");
  const columnItems = () => spanItems("column", sheet.wide(), "left", "right");
  const isHead = () =>
    active().row === headOf(sheet.tab()) &&
    active().column <= sheet.tab().columns.length;
  // A file that is only read keeps its filter and no other command.
  const menu = (at, items) => {
    const shown = items.filter(Boolean);
    if (shown.length) openMenu(at, shown, hold);
  };
  const changes = (items) => (sheet.locked ? [] : items);
  const cellItems = () => [
    ...(isHead() ? headItems() : []),
    ...changes([
      ...rowItems(),
      ...columnItems(),
      command("Insert chart", "chart"),
    ]),
  ];
  const under = () => {
    const box = cellAt(sheet.table(), active()).getBoundingClientRect();
    return { x: box.left, y: box.bottom };
  };

  editor.addEventListener("keydown", (event) => {
    const { key } = event;
    const held = event.ctrlKey || event.metaKey;
    const letter = held && key.toLowerCase();
    const { anchor, focus } = sheet.pick();
    const end = last();
    // With Ctrl an arrow stops at the table's last row or column; from
    // there, or from outside the table, it runs to the grid's.
    const run = (by) =>
      focus.row <= end.row &&
      focus.column <= end.column &&
      (by.row > 0 ? focus.row < end.row : focus.column < end.column)
        ? { rows: end.row, columns: end.column }
        : sheet.size();
    const way = event.shiftKey ? -1 : 1;
    if (editing) {
      if (key === "Enter") commit({ row: way, column: 0 });
      else if (key === "Tab") commit({ row: 0, column: way });
      else if (key === "Escape") rest();
      else return;
    } else if (held && (key === "PageUp" || key === "PageDown"))
      sheet.say({ type: "key", key, held });
    else if (event.altKey && key === "ArrowDown") {
      if (isHead()) menu(under(), headItems());
      else edit();
    } else if (ARROWS[key])
      sheet.select(
        held
          ? edgeFrom(
              (at) => !!cell(at).text,
              run(ARROWS[key]),
              focus,
              ARROWS[key],
            )
          : stepped(focus, ARROWS[key]),
        event.shiftKey,
      );
    else if (key === "Tab")
      sheet.select(stepped(anchor, { row: 0, column: way }));
    else if (key === "Enter" || key === "F2") edit();
    else if (key === "Home")
      sheet.select({ row: held ? 1 : focus.row, column: 1 }, event.shiftKey);
    else if (key === "End")
      sheet.select(
        { row: held ? end.row : focus.row, column: end.column },
        event.shiftKey,
      );
    else if (key === "PageUp" || key === "PageDown") {
      const page = Math.max(
        1,
        Math.floor(innerHeight / editor.offsetHeight) - 2,
      );
      sheet.select(
        { ...focus, row: focus.row + (key === "PageUp" ? -page : page) },
        event.shiftKey,
      );
    } else if (key === "Delete" || key === "Backspace") sheet.run("clear");
    else if (letter === "z") sheet.run(event.shiftKey ? "redo" : "undo");
    else if (letter === "y") sheet.run("redo");
    else if (letter === "d") sheet.run("fill-down");
    else if (letter === "a") sheet.select(end, { row: 1, column: 1 }, true);
    else if (key === " " && isBox()) toggle();
    else if (key === "ContextMenu" || (event.shiftKey && key === "F10"))
      menu(under(), cellItems());
    else return;
    event.preventDefault();
  });

  // A row's number has no column, a column's letter no row.
  const hit = (event) => {
    const drawn = event.target.closest?.("[data-row], [data-column]");
    return drawn && sheet.table().contains(drawn)
      ? {
          row: Number(drawn.dataset.row ?? 0),
          column: Number(drawn.dataset.column ?? 0),
        }
      : undefined;
  };
  let dragging = false;
  addEventListener("mousedown", (event) => {
    const at = hit(event);
    if (!at || event.button) return;
    // The field keeps the focus.
    event.preventDefault();
    if (editing) commit();
    // A row's number or a column's letter selects it across the table.
    if (at.row && at.column) sheet.select(at, event.shiftKey);
    else if (at.row)
      sheet.select(
        { row: at.row, column: last().column },
        { row: at.row, column: 1 },
        true,
      );
    else if (at.column)
      sheet.select(
        { row: last().row, column: at.column },
        { row: 1, column: at.column },
        true,
      );
    dragging = !!(at.row && at.column);
    hold();
  });
  addEventListener("mouseover", (event) => {
    const at = hit(event);
    if (dragging && event.buttons === 1 && at?.row && at.column)
      sheet.select(at, true);
  });
  addEventListener("mouseup", () => {
    dragging = false;
    if (document.activeElement === document.body) hold();
  });
  addEventListener("focus", hold);
  addEventListener("click", (event) => {
    const at = hit(event);
    const point = { x: event.clientX, y: event.clientY };
    if (!at) return;
    if (event.target.closest(".sheet-more")) menu(point, headItems());
    else if (event.target.type === "checkbox") {
      event.preventDefault();
      toggle();
    } else if (!at.column) menu(point, changes(rowItems()));
    else if (!at.row) menu(point, changes(columnItems()));
  });
  addEventListener("dblclick", (event) => {
    const at = hit(event);
    if (at?.row && at.column && event.target.type !== "checkbox") edit();
  });
  addEventListener("contextmenu", (event) => {
    const at = hit(event);
    if (!at?.row || !at.column) return;
    event.preventDefault();
    const { top, left, bottom, right } = sheet.box();
    if (
      at.row < top ||
      at.row > bottom ||
      at.column < left ||
      at.column > right
    )
      sheet.select(at);
    menu({ x: event.clientX, y: event.clientY }, cellItems());
  });

  // Cells are copied as what they show, and pasted back here as what was
  // typed, formulas and all.
  const cells = (read) => {
    const { top, left, bottom, right } = sheet.box();
    return Array.from({ length: bottom - top + 1 }, (_, r) =>
      Array.from({ length: right - left + 1 }, (_, c) =>
        read({ row: top + r, column: left + c }),
      ),
    );
  };
  for (const kind of ["copy", "cut"])
    addEventListener(kind, (event) => {
      if (editing || event.target !== editor) return;
      const text = toTsv(
        cells(({ row, column }) => book.shown(sheet.at, row, column)),
      );
      event.clipboardData.setData("text/plain", text);
      event.preventDefault();
      copied = {
        text,
        tabs: book.tabs,
        rows: cells((at) => cell(at).text),
        from: { sheet: sheet.at, box: sheet.box(), cut: kind === "cut" },
      };
    });
  addEventListener("paste", (event) => {
    if (editing || event.target !== editor) return;
    event.preventDefault();
    const text = event.clipboardData.getData("text/plain");
    if (!text) return;
    // After any change the cells copied may lie elsewhere: only text is left.
    const own =
      copied?.text === text && copied.tabs === book.tabs ? copied : undefined;
    const rows = own?.rows ?? fromTsv(text);
    const { top, left } = sheet.box();
    sheet.did(
      book.paste(sheet.at, { row: top, column: left }, rows, own?.from),
    );
    sheet.select(
      { row: top + rows.length - 1, column: left + rows[0].length - 1 },
      { row: top, column: left },
    );
    if (own?.from.cut) copied = undefined;
  });

  return { rest, hold, typing: () => editing };
}
