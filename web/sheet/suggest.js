// What a formula being typed may take at its caret: the functions named by
// the letters there, or the arguments of the call it stands in. Drawn under
// the cell typed in; the app draws the same under its formula bar.

import { FUNCTIONS } from "./functions.js";

const KNOWN = new Map(
  FUNCTIONS.trim()
    .split("\n")
    .map((line) => {
      const [, name, args, about] = /^([^( ]+)(?:\((.*?)\))? ?(.*)$/.exec(line);
      return [name, { name, args: args?.split(", ").filter(Boolean), about }];
    }),
);

// `text` is the formula up to its caret, `tokens` reads it as the engine
// does. The functions whose name starts with the letters typed come first,
// then, past one letter, those that hold them; `typed` is how many letters
// one takes the place of. Without any, the call's arguments and the one
// reached.
export function suggestions(text, tokens) {
  if (!text.startsWith("=")) return {};
  const read = tokens(text).map(({ token }) => token);
  // The brackets left open: a call's has its name; in braces a comma is
  // not the call's.
  const open = [];
  read.forEach((token, at) => {
    if (token === "LeftParenthesis")
      open.push({ name: read[at - 1]?.Ident?.toUpperCase(), at: 0 });
    else if (token === "LeftBrace") open.push({ at: 0 });
    else if (token === "RightParenthesis" || token === "RightBrace") open.pop();
    else if (token === "Comma" && open.length) open.at(-1).at++;
  });
  // After a colon letters are a range's end ("A1:B"), after a digit a
  // number's ("1e").
  const [before, { Ident: typed } = {}] = read.slice(-2);
  const letters =
    typed &&
    before !== "Colon" &&
    text.endsWith(typed) &&
    !/\d$/.test(text.slice(0, -typed.length)) &&
    typed.toUpperCase();
  const all = letters ? [...KNOWN.values()] : [];
  const options = [
    ...all.filter(({ name }) => name.startsWith(letters)),
    ...all.filter(({ name }) => typed.length > 1 && name.indexOf(letters) > 0),
  ].map(({ name, about }) => ({ name, about }));
  if (options.length) return { typed: typed.length, options };
  const call = open.findLast(({ name }) => name);
  const args = KNOWN.get(call?.name)?.args;
  if (!args) return {};
  const at = args.at(-1) === "…" ? Math.min(call.at, args.length - 1) : call.at;
  return { hint: { name: call.name, args, at } };
}

// A key that only confirms what an input method composed; in WebKit that
// one comes after the composition has ended.
export const composing = (event) => event.isComposing || event.keyCode === 229;

// `field` is the cell's editor: while a list is open under it, the arrows
// walk the list, Enter and Tab take from it, and Escape shuts it.
export function suggest(sheet, field) {
  const box = document.createElement("div");
  box.className = "sheet-suggest";
  box.id = "sheet-suggest";
  field.setAttribute("aria-autocomplete", "list");
  // What stands before the caret, as last looked at, and what it may take.
  let seen = "";
  let found = {};
  let active = 0;
  const shut = () => {
    found = {};
    box.remove();
    for (const name of ["activedescendant", "controls", "describedby"])
      field.removeAttribute(`aria-${name}`);
  };
  // Under the cell, or over it where there is more room: never on it, and
  // no higher than the room there.
  const place = () => {
    if (!box.isConnected) return;
    const cell = field.getBoundingClientRect();
    box.style.removeProperty("--sheet-room");
    const below = innerHeight - cell.bottom;
    const under = box.offsetHeight <= below || below >= cell.top;
    box.style.setProperty("--sheet-room", `${under ? below : cell.top}px`);
    box.style.top = `${scrollY + (under ? cell.bottom : cell.top - box.offsetHeight)}px`;
    box.style.left = `${scrollX + Math.max(0, Math.min(cell.left, innerWidth - box.offsetWidth))}px`;
  };
  const mark = () => {
    [...box.children].forEach((row, at) => {
      row.setAttribute("aria-selected", at === active);
    });
    const row = box.children[active];
    box.scrollTop = Math.min(
      row.offsetTop,
      Math.max(
        box.scrollTop,
        row.offsetTop + row.offsetHeight - box.clientHeight,
      ),
    );
    field.setAttribute("aria-activedescendant", row.id);
  };
  const strong = (text) =>
    Object.assign(document.createElement("b"), { textContent: text });
  const show = () => {
    const caret = field.selectionStart;
    // Typed in the app's bar, the caret is there and so is the list.
    const text =
      document.hasFocus() &&
      "editing" in field.dataset &&
      field.selectionEnd === caret
        ? field.value.slice(0, caret)
        : "";
    if (text === seen) return;
    seen = text;
    found = suggestions(text, sheet.book.tokens);
    active = 0;
    const { options, hint } = found;
    if (!options && !hint) return shut();
    box.setAttribute("role", options ? "listbox" : "note");
    box.ariaLabel = options ? "Functions" : null;
    box.replaceChildren(
      ...(options?.map(({ name, about }, at) => {
        const row = document.createElement("div");
        row.id = `sheet-suggest-${at}`;
        row.setAttribute("role", "option");
        row.append(strong(name), ` ${about}`);
        return row;
      }) ?? [
        `${hint.name}(`,
        ...hint.args.flatMap((arg, at) => [
          at ? ", " : "",
          at === hint.at ? strong(arg) : arg,
        ]),
        ")",
      ]),
    );
    document.body.append(box);
    // The field names what it offers: a list to walk, or a note to hear.
    field.setAttribute(options ? "aria-controls" : "aria-describedby", box.id);
    field.removeAttribute(options ? "aria-describedby" : "aria-controls");
    if (options) mark();
    else field.removeAttribute("aria-activedescendant");
    place();
  };
  // A bracket that already follows the caret is the name's.
  const take = ({ name }) => {
    const caret = field.selectionStart;
    const to = caret + (field.value[caret] === "(");
    field.setRangeText(`${name}(`, caret - found.typed, to, "end");
    field.dispatchEvent(new Event("input"));
  };
  // The field keeps the focus: a press on the list is no blur.
  box.addEventListener("mousedown", (event) => event.preventDefault());
  box.addEventListener("click", (event) => {
    const row = event.target.closest("[role=option]");
    if (row) take(found.options[[...box.children].indexOf(row)]);
  });
  // Before the field's own keys: an open list has them first.
  addEventListener(
    "keydown",
    (event) => {
      const { options } = found;
      const held =
        event.shiftKey || event.ctrlKey || event.metaKey || event.altKey;
      const by = { ArrowDown: 1, ArrowUp: -1 }[event.key];
      if (event.target !== field || !options || held || composing(event))
        return;
      if (by) {
        active = (active + by + options.length) % options.length;
        mark();
      } else if (event.key === "Enter" || event.key === "Tab")
        take(options[active]);
      else if (event.key === "Escape") shut();
      else return;
      event.preventDefault();
      event.stopPropagation();
    },
    true,
  );
  // The caret moved by an arrow or a press is at another argument.
  document.addEventListener(
    "selectionchange",
    () => document.activeElement === field && show(),
  );
  return { show, place };
}
