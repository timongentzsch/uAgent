// One field holds the focus, unseen on the active cell, so typing replaces
// and the clipboard works as in any field.

import { headOf } from "./book.js";
import { actionsOf } from "./actions.js";
import { openMenu } from "./menu.js";
import { composing, suggest } from "./suggest.js";
import { edgeFrom, escaped, fromTsv, LAST, toTsv } from "./text.js";

const ARROWS = {
  ArrowUp: { row: -1, column: 0 },
  ArrowDown: { row: 1, column: 0 },
  ArrowLeft: { row: 0, column: -1 },
  ArrowRight: { row: 0, column: 1 },
};

export function listen(sheet) {
  const { book } = sheet;
  const editor = Object.assign(document.createElement("input"), {
    className: "sheet-editor",
    ariaLabel: "Cell",
    autocomplete: "off",
    spellcheck: false,
  });
  document.body.append(editor);
  const hints = suggest(sheet, editor);
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
    const box = sheet.rect(active());
    Object.assign(field.style, {
      left: `${box.left}px`,
      top: `${box.top}px`,
      width: `${box.width}px`,
      height: `${box.height}px`,
    });
  };

  // What is being typed is said as it is typed, as the file would hold it;
  // first with the table it would make, cut around it, which the app keeps
  // if the page is left before the cell is. It is said to be over where no
  // edit follows, and where it is the cell's text again.
  let drafted = false;
  const draft = () => {
    const text = editor.value;
    if (text === cell().text) return undraft();
    sheet.say({
      type: "typing",
      text,
      written: escaped(text),
      ...(drafted ? {} : sheet.draft()),
    });
    drafted = true;
  };
  const undraft = () => {
    if (drafted) sheet.say({ type: "typing" });
    drafted = false;
  };
  const rest = () => {
    undraft();
    editing = false;
    const gone = list;
    list = undefined;
    gone?.remove();
    place(editor);
    delete editor.dataset.editing;
    hints.show();
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
    sheet.see();
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
    const said = drafted;
    drafted = false;
    rest();
    sheet.write(text);
    // Written or refused, that draft is over: the next has its own table.
    if (said) sheet.say({ type: "typing" });
    if (by) sheet.select(stepped(active(), by));
  };
  editor.addEventListener("input", () => {
    if (!editing) edit(true);
    if (editing) draft();
    hints.show();
  });
  // What is typed in the app's bar is typed in the cell: shown in it, said
  // back as its draft, and written when the bar is left or the page is.
  const type = (text) => {
    if (sheet.locked) return;
    editing = true;
    sheet.see();
    editor.dataset.editing = "";
    editor.value = text;
    draft();
    hints.show();
  };
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

  const isHead = () =>
    active().row === headOf(sheet.tab()) &&
    active().column <= sheet.tab().columns.length;
  const menu = (at, items) => items.length && openMenu(at, items, hold);
  const under = () => {
    const box = sheet.rect(active());
    return { x: box.left - scrollX, y: box.top + box.height - scrollY };
  };

  editor.addEventListener("keydown", (event) => {
    if (composing(event)) return;
    const { key } = event;
    const held = event.ctrlKey || event.metaKey;
    const letter = held && key.toLowerCase();
    const { anchor, focus } = sheet.pick();
    const end = last();
    // With Ctrl an arrow stops at the table's last row or column; from
    // there, or from outside the table, it runs to the spreadsheet's.
    const run = (by) =>
      focus.row <= end.row &&
      focus.column <= end.column &&
      (by.row > 0 ? focus.row < end.row : focus.column < end.column)
        ? { rows: end.row, columns: end.column }
        : { rows: LAST.row, columns: LAST.column };
    const way = event.shiftKey ? -1 : 1;
    if (editing) {
      if (key === "Enter") commit({ row: way, column: 0 });
      else if (key === "Tab") commit({ row: 0, column: way });
      else if (key === "Escape") rest();
      else return;
    } else if (held && (key === "PageUp" || key === "PageDown"))
      sheet.say({ type: "key", key, held });
    else if (event.altKey && key === "ArrowDown") {
      if (isHead()) menu(under(), actions("head"));
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
    else if (key === "Home") {
      sheet.select({ row: held ? 1 : focus.row, column: 1 }, event.shiftKey);
      sheet.start(held);
    } else if (key === "End")
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
    else if (letter === "b") sheet.run("bold");
    else if (letter === "i") sheet.run("italic");
    else if (letter === "a") sheet.select(end, { row: 1, column: 1 }, true);
    else if (key === " " && isBox()) toggle();
    else if (key === "ContextMenu" || (event.shiftKey && key === "F10"))
      menu(under(), actions("cell"));
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
  // A row's number or a column's letter selects it across the table.
  const picked = (at, extend) => {
    if (at.row && at.column) sheet.select(at, extend);
    else if (at.row)
      sheet.select(
        { row: at.row, column: last().column },
        { row: at.row, column: 1 },
        true,
      );
    else
      sheet.select(
        { row: last().row, column: at.column },
        { row: 1, column: at.column },
        true,
      );
  };
  // A column's edge dragged between two letters is its width: a line shows
  // where it will stand, and letting go writes it.
  const resize = (event, column) => {
    const from = sheet.rect({ row: 1, column });
    const guide = Object.assign(document.createElement("div"), {
      className: "sheet-guide",
    });
    const width = (to) =>
      Math.max(40, Math.round(from.width + to.clientX - event.clientX));
    const move = (to) => {
      guide.style.left = `${from.left - scrollX + width(to)}px`;
    };
    document.body.append(guide);
    move(event);
    addEventListener("mousemove", move);
    addEventListener(
      "mouseup",
      (to) => {
        removeEventListener("mousemove", move);
        guide.remove();
        if (to.clientX !== event.clientX)
          sheet.did(
            book.widen(sheet.at, { left: column, right: column }, width(to)),
          );
      },
      { once: true },
    );
  };
  let dragging = false;
  addEventListener("mousedown", (event) => {
    const at = hit(event);
    if (!at || event.button) return;
    // The field keeps the focus.
    event.preventDefault();
    if (editing) commit();
    if (event.target.closest(".sheet-resize")) return resize(event, at.column);
    picked(at, event.shiftKey);
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
    if (event.target.closest(".sheet-more")) menu(point, actions("head"));
    else if (event.target.type === "checkbox") {
      event.preventDefault();
      toggle();
    } else if (event.target.closest(".sheet-resize")) return;
    else if (!at.column) menu(point, actions("row"));
    else if (!at.row) menu(point, actions("column"));
  });
  // Twice on a column's edge fits it to its texts again.
  addEventListener("dblclick", (event) => {
    const at = hit(event);
    if (event.target.closest(".sheet-resize"))
      sheet.did(book.widen(sheet.at, { left: at.column, right: at.column }));
    else if (at?.row && at.column && event.target.type !== "checkbox") edit();
  });
  // On a cell, a row's number or a column's letter: what was selected
  // stays where the press lies in it.
  addEventListener("contextmenu", (event) => {
    const at = hit(event);
    if (!at) return;
    event.preventDefault();
    const { top, left, bottom, right } = sheet.box();
    // A row's number lies in a selection of whole rows, a letter of columns.
    const inside =
      (at.row
        ? at.row >= top && at.row <= bottom
        : top <= 1 && bottom >= last().row) &&
      (at.column
        ? at.column >= left && at.column <= right
        : left <= 1 && right >= last().column);
    if (!inside) picked(at);
    menu(
      { x: event.clientX, y: event.clientY },
      actions(!at.column ? "row" : !at.row ? "column" : "cell"),
    );
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
  const pasted = (text) => {
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
  };
  addEventListener("paste", (event) => {
    if (editing || event.target !== editor) return;
    event.preventDefault();
    const text = event.clipboardData.getData("text/plain");
    if (text) pasted(text);
  });
  // From a menu the field's own commands copy; only what was copied here
  // can be pasted, since the page cannot read the clipboard.
  const actions = actionsOf(sheet, {
    copied: () => copied,
    cut: () => document.execCommand("cut"),
    copy: () => document.execCommand("copy"),
    paste: () => pasted(copied.text),
  });

  return {
    rest,
    hold,
    place() {
      place(list ?? editor);
      hints.place();
    },
    type,
    commit: () => editing && commit(),
  };
}
