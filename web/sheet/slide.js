// A slide changed by hand in the app while Edit is on (items.js): what a
// press picks is moved, resized, copied, laid over or under and deleted,
// what a tool makes is put in, and a press again on the one picked types
// in its text by the helper's edits. A move or a size is the element's own
// style in the slide's pixels (translate, width, height), so the rest of the
// slide stays where it is. Each change is said for the app to write
// (slideEdit, doc-edit.ts).

import { APART, itemLayer, rectOf } from "./items.js";

// What a press picks: the outermost of these that holds it.
const ITEMS =
  "h1, h2, h3, h4, h5, h6, p, ul, ol, dl, table, blockquote, pre, figure, img, svg, [data-shape]";
// What each tool makes, at what size; a shape is filled with the accent.
const MADE = {
  text: { tag: "p", size: [480, 0] },
  table: { tag: "table", size: [640, 0] },
  rect: { tag: "div", size: [240, 160], radius: "" },
  round: { tag: "div", size: [240, 160], radius: "24px" },
  ellipse: { tag: "div", size: [240, 160], radius: "50%" },
};
const TABLE = `<thead><tr>${"<th>Column</th>".repeat(2)}</tr></thead><tbody>${`<tr>${"<td>Cell</td>".repeat(2)}</tr>`.repeat(2)}</tbody>`;

const squash = (text) => text.replace(/\s+/g, "");
const px = (value) => Math.round(Number.parseFloat(value));
const shaped = (item) => item.matches("[data-shape]");
// Put in by hand: placed by its own style, not by the flow.
const placed = (item) => item.style.position === "absolute";
// Where the element was moved from its place, by its translate.
const moved = (item) => {
  const [x = 0, y = 0] = item.style.translate
    .split(/\s+/)
    .filter(Boolean)
    .map(px);
  return [x, y];
};
const translate = (item, [x, y]) => {
  item.style.translate = x || y ? `${x}px ${y}px` : "";
  if (!item.style.length) item.removeAttribute("style");
};
// An element's box in the page.
const box = (item) => {
  const { left, top, width, height } = item.getBoundingClientRect();
  return [left + scrollX, top + scrollY, width, height];
};
// The size its style gives for a box this size.
const inner = (item, w, h) => {
  const own = getComputedStyle(item);
  const edge = (a, b) =>
    own.boxSizing === "border-box"
      ? 0
      : [`padding${a}`, `padding${b}`, `border${a}Width`, `border${b}Width`]
          .map((side) => Number.parseFloat(own[side]))
          .reduce((sum, side) => sum + side, 0);
  return [
    Math.round(w - edge("Left", "Right")),
    Math.round(h - edge("Top", "Bottom")),
  ];
};

let hand;
let off;
let typeIn;
let typing;
// Whether the slide is changed by hand now, and whether a person is
// holding or nudging an element.
export const editing = () => !!hand;
export const busy = () => !!hand?.busy();
// A press, or the app, types in a block: until a press elsewhere.
export function type(block, press, over) {
  typing = block;
  typeIn(block, press, over);
}
const stop = () => {
  typing?.blur();
  typing?.removeAttribute("contenteditable");
  typing = undefined;
};

// On: the slide's elements picked by a press; off, as written. The helper
// gives what it says with, the path of an element, the notes, the text
// blocks and what makes one, typing in one and keeping a new one's text.
export function edit(on, helper) {
  if (!on) {
    stop();
    hand?.end();
    off?.abort();
    hand = undefined;
    return;
  }
  if (hand) return hand.hold();
  const { send, selector, notes, blocks, remember } = helper;
  typeIn = helper.typeIn;
  off = new AbortController();
  // A press elsewhere ends the typing.
  addEventListener(
    "pointerdown",
    (event) => typing && !typing.contains(event.target) && stop(),
    { capture: true, signal: off.signal },
  );
  // Where its content is: the one element that wraps it, or the body.
  const root = () => {
    const [only, more] = [...document.body.children].filter(
      (part) => !part.matches("aside, script, style"),
    );
    return only && !more && !only.matches(helper.text) ? only : document.body;
  };
  const inside = (item) =>
    item &&
    item !== root() &&
    root().contains(item) &&
    !notes()?.contains(item);
  const items = () =>
    [...root().querySelectorAll(ITEMS)].filter(
      (item) => inside(item) && !item.parentElement.closest(ITEMS),
    );
  const itemOf = (target) => {
    let item;
    for (
      let at = target.closest?.(ITEMS);
      at;
      at = at.parentElement?.closest(ITEMS)
    )
      item = at;
    return inside(item) ? item : undefined;
  };
  // An element as the app finds it in the file: where it is and what it
  // holds.
  const address = (item) => ({
    path: selector(item),
    tag: item.localName,
    text: squash(item.textContent),
  });
  const say = (change) => {
    remember();
    send("change", change);
  };
  // Placed by its own style with its box's corner at (x, y), `size` big:
  // where that is in what it is placed in, and its size, as said.
  const place = (made, x, y, size) => {
    const [width, height] = inner(made, ...size);
    const shape = shaped(made) && { height };
    Object.assign(made.style, {
      position: "absolute",
      left: "0px",
      top: "0px",
      width: `${width}px`,
      ...(shape && { height: `${height}px` }),
    });
    const [left, top] = box(made).map((at, index) =>
      Math.round([x, y][index] - at),
    );
    Object.assign(made.style, { left: `${left}px`, top: `${top}px` });
    return { place: [left, top], width, ...shape };
  };
  // What a tool makes, where the slide was pressed, picked.
  const insert = (tool, event) => {
    const kind = MADE[tool];
    if (!kind) return false;
    const made = document.createElement(kind.tag);
    if (tool === "text") made.textContent = "Text";
    if (tool === "table") made.innerHTML = TABLE;
    if (kind.radius !== undefined) {
      made.dataset.shape = tool;
      made.style.background = "var(--accent, currentcolor)";
      made.style.borderRadius = kind.radius;
    }
    const aside = notes();
    if (aside?.parentElement === root()) aside.before(made);
    else root().append(made);
    const [x, y] = [event.clientX + scrollX, event.clientY + scrollY];
    say({ add: { kind: tool, ...place(made, x, y, kind.size) } });
    layer.pick([made]);
    if (tool === "text") type(made, undefined, true);
    return true;
  };
  const layer = itemLayer({
    touch: ITEMS,
    items,
    itemOf,
    box,
    put: (item, [x, y, w, h], resized) => {
      if (resized) {
        const [width, height] = inner(item, w, h);
        item.style.width = `${width}px`;
        if (shaped(item)) item.style.height = `${height}px`;
      }
      const [bx, by] = box(item);
      const [tx, ty] = moved(item);
      translate(item, [Math.round(tx + x - bx), Math.round(ty + y - by)]);
    },
    tall: shaped,
    said: (list, resized) => ({
      set: list.map((item) => ({
        at: address(item),
        move: moved(item),
        ...(resized && {
          width: px(item.style.width),
          ...(shaped(item) && { height: px(item.style.height) }),
        }),
      })),
    }),
    // Each copy placed by hand where its original is, a little further on.
    copy: (list) => {
      const at = list.map(address);
      const made = list.map((item) => {
        const [x, y, w, h] = box(item);
        const copied = item.cloneNode(true);
        for (const part of [copied, ...copied.querySelectorAll("*")])
          for (const name of [
            "id",
            "contenteditable",
            "data-uagent-picked",
            "data-uagent-changed",
          ])
            part.removeAttribute(name);
        item.after(copied);
        translate(copied, [0, 0]);
        return [copied, place(copied, x + APART, y + APART, [w, h])];
      });
      return [
        made.map(([copied]) => copied),
        {
          copy: made.map(([, placed], index) => ({
            at: at[index],
            ...placed,
          })),
        },
      ];
    },
    // Of what was put in by hand, over or under the rest put in beside it.
    order: (picked, over) => {
      const list = picked.filter(placed);
      const at = list.map(address);
      let changed = false;
      for (const item of list) {
        const peers = [...item.parentElement.children].filter(placed);
        const end = over ? peers.at(-1) : peers[0];
        if (end === item) continue;
        end[over ? "after" : "before"](item);
        changed = true;
      }
      if (changed) return { [over ? "front" : "back"]: at };
    },
    remove: (list) => {
      const at = list.map(address);
      for (const item of list) item.remove();
      return { remove: at };
    },
    retype: (item, press) => {
      const block =
        blocks().find((one) => one.contains(press.target)) ??
        blocks().find((one) => item.contains(one));
      if (block) type(block, press);
    },
    insert,
    act: (act) => {
      const [one, more] = layer.picked();
      if (!one || more) return false;
      // Its container, picked; or its own place and size again.
      if (act === "container" && inside(one.parentElement))
        layer.pick([one.parentElement]);
      else if (act === "reset" && !placed(one)) {
        for (const name of ["translate", "width", "height"])
          one.style.removeProperty(name);
        if (!one.style.length) one.removeAttribute("style");
        say({
          set: [{ at: address(one), move: [0, 0], width: null, height: null }],
        });
        layer.hold();
      } else return false;
      return true;
    },
    show: (picked) =>
      send("items", {
        items: picked.map((item) => ({
          selector: selector(item),
          tag: item.localName,
          text: item.textContent.trim().slice(0, 60),
          shape: item.dataset.shape,
          container: inside(item.parentElement),
          moved:
            !placed(item) &&
            ["translate", "width", "height"].some((name) => item.style[name]),
          placed: placed(item),
        })),
        rect: rectOf(picked),
      }),
    say,
  });
  hand = layer;
}
