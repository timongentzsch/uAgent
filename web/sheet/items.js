// Items laid on a page by `data-at="x y"` and `data-size="w h"`, changed by
// hand in the app. A press picks one, with Shift adds or drops it; a drag
// from the empty page frames the items it touches (on a touch screen it
// scrolls); a drag moves the picked together and a corner resizes one; the
// keys nudge, delete, duplicate, copy and paste, pick all and let go; a
// double click retypes an item's text. Each change is said once, for the
// app to write (board-edit.ts).

const CSS = `
[data-at] { touch-action: none; }
[data-uagent-picked] { outline: 2px solid #1a56db; outline-offset: 2px; }
.uagent-handle { position: absolute; width: 10px; height: 10px; margin: -5px;
  z-index: 3; background: #1a56db; }
.uagent-frame { position: absolute; z-index: 3; border: 1px solid #1a56db;
  background: #1a56db1a; }
`;
// The corners, as the sides of the item each one moves.
const CORNERS = [
  [-1, -1],
  [1, -1],
  [-1, 1],
  [1, 1],
];
// How far a copy lies from what it copies, and the smallest an item gets.
const APART = 20;
const LEAST = 20;

export const numbers = (text) =>
  (text ?? "")
    .split(/[\s,]+/)
    .filter(Boolean)
    .map(Number);
export const items = () =>
  [...document.body.children].filter((element) =>
    element.hasAttribute("data-at"),
  );
export const grid = (n) => Math.round(n / 10) * 10;
const at = (item) => [item.offsetLeft, item.offsetTop];
// An id no element has yet, made from `stem`.
export function fresh(stem) {
  const base = stem.toLowerCase().replace(/[^a-z0-9-]|\d+$/g, "") || "item";
  let n = 1;
  while (document.getElementById(`${base}${n}`)) n++;
  return `${base}${n}`;
}

// Whether a person is changing an item: holding, nudging or retyping it.
let changing = false;
export const busy = () =>
  changing || !!document.activeElement?.isContentEditable;

// `place` lays an item as its attributes say, `drawn` follows a change (a
// board's arrows), `say` tells the app a change, `show` what is picked, and
// `remove` takes an item away. Gives what keeps the corners on the picked,
// the pick, and the changes the app asks of what is picked.
export function itemLayer({ place, drawn, say, show, remove }) {
  const style = document.createElement("style");
  style.textContent = CSS;
  document.head.append(style);
  let picked = [];
  const handles = CORNERS.map(([sx, sy]) => {
    const handle = document.createElement("span");
    handle.className = "uagent-handle";
    handle.dataset.uagent = "";
    handle.style.cursor = sx === sy ? "nwse-resize" : "nesw-resize";
    return handle;
  });
  const hold = () => {
    picked = picked.filter((item) => item.isConnected);
    const [one, more] = picked;
    handles.forEach((handle, index) => {
      if (!one || more) return handle.remove();
      const [sx, sy] = CORNERS[index];
      handle.style.left = `${one.offsetLeft + (sx > 0 ? one.offsetWidth : 0)}px`;
      handle.style.top = `${one.offsetTop + (sy > 0 ? one.offsetHeight : 0)}px`;
      if (!handle.isConnected) document.body.append(handle);
    });
    show(picked);
  };
  const pick = (next) => {
    for (const item of picked) item.removeAttribute("data-uagent-picked");
    picked = next.filter((item) => item.id);
    for (const item of picked) item.setAttribute("data-uagent-picked", "");
    hold();
  };
  const changed = (list) => {
    for (const item of list) place(item);
    drawn();
    hold();
  };
  const placed = (list) =>
    say({ set: list.map((item) => ({ id: item.id, at: at(item) })) });
  const moveTo = (list, places) => {
    list.forEach((item, index) => {
      item.dataset.at = places[index].join(" ");
    });
    changed(list);
  };

  // What a drag does as it goes, and once it is let go.
  const move = (list) => {
    const starts = list.map(at);
    const [x, y] = starts[0];
    return [
      (dx, dy) =>
        moveTo(
          list,
          starts.map(([sx, sy]) => [
            sx + grid(x + dx) - x,
            sy + grid(y + dy) - y,
          ]),
        ),
      () => placed(list),
    ];
  };
  const resize = (item, [sx, sy]) => {
    const [x, y, w, h] = [...at(item), item.offsetWidth, item.offsetHeight];
    // A span with its start (side < 0) or its end moved by d.
    const side = (start, size, toward, d) => {
      if (toward > 0) return [start, Math.max(LEAST, grid(size + d))];
      const moved = Math.min(grid(start + d), start + size - LEAST);
      return [moved, start + size - moved];
    };
    return [
      (dx, dy) => {
        const [left, width] = side(x, w, sx, dx);
        const [top, height] = side(y, h, sy, dy);
        item.dataset.at = `${left} ${top}`;
        item.dataset.size = `${width} ${height}`;
        changed([item]);
      },
      () =>
        say({
          set: [
            { id: item.id, at: at(item), size: numbers(item.dataset.size) },
          ],
        }),
    ];
  };
  const frame = ([x, y], adding) => {
    const was = adding ? picked : [];
    let box;
    return [
      (dx, dy) => {
        const [left, top] = [Math.min(x, x + dx), Math.min(y, y + dy)];
        const [width, height] = [Math.abs(dx), Math.abs(dy)];
        if (!box) {
          box = document.createElement("div");
          box.className = "uagent-frame";
          box.dataset.uagent = "";
          document.body.append(box);
        }
        Object.assign(box.style, {
          left: `${left}px`,
          top: `${top}px`,
          width: `${width}px`,
          height: `${height}px`,
        });
        const touched = items().filter(
          (item) =>
            !was.includes(item) &&
            item.offsetLeft < left + width &&
            left < item.offsetLeft + item.offsetWidth &&
            item.offsetTop < top + height &&
            top < item.offsetTop + item.offsetHeight,
        );
        pick([...was, ...touched]);
      },
      () => box?.remove(),
    ];
  };
  addEventListener("pointerdown", (event) => {
    if (event.button || event.target.isContentEditable) return;
    // A press elsewhere ends the typing in an item, which saves it.
    if (document.activeElement?.isContentEditable)
      document.activeElement.blur();
    const corner = handles.indexOf(event.target);
    const item = event.target.closest?.("[data-at]");
    const start = [event.clientX, event.clientY];
    let drag;
    if (corner >= 0) drag = resize(picked[0], CORNERS[corner]);
    else if (item?.id && event.shiftKey)
      pick(
        picked.includes(item)
          ? picked.filter((other) => other !== item)
          : [...picked, item],
      );
    else if (item?.id) {
      if (!picked.includes(item)) pick([item]);
      drag = move(picked);
    } else if (event.pointerType === "touch") return pick([]);
    else {
      if (!event.shiftKey) pick([]);
      drag = frame(start, event.shiftKey);
    }
    // No text is selected by a press; the page still takes the keys.
    event.preventDefault();
    focus();
    if (!drag) return;
    changing = true;
    let moved = false;
    const off = new AbortController();
    const follow = (next) => {
      const [dx, dy] = [next.clientX - start[0], next.clientY - start[1]];
      moved ||= Math.hypot(dx, dy) > 3;
      if (moved) drag[0](dx, dy);
    };
    const drop = () => {
      off.abort();
      changing = false;
      if (moved) drag[1]();
    };
    addEventListener("pointermove", follow, { signal: off.signal });
    addEventListener("pointerup", drop, { signal: off.signal });
    addEventListener("pointercancel", drop, { signal: off.signal });
  });

  // Copies of the items, over the rest, picked.
  const copy = (list) => {
    if (!list.length) return;
    const made = list.map((item) => {
      const copied = item.cloneNode(true);
      copied.removeAttribute("data-uagent-picked");
      copied.removeAttribute("data-uagent-changed");
      copied.id = fresh(item.id);
      const [x, y] = at(item);
      copied.dataset.at = `${x + APART} ${y + APART}`;
      items().at(-1).after(copied);
      return copied;
    });
    say({
      copy: list.map((item, index) => ({
        id: item.id,
        as: made[index].id,
        at: numbers(made[index].dataset.at),
      })),
    });
    changed(made);
    pick(made);
  };
  // The picked laid over the rest, or under them.
  const order = (over) => {
    const list = items().filter((item) => picked.includes(item));
    const rest = items().filter((item) => !picked.includes(item));
    if (!list.length || !rest.length) return;
    if (over) rest.at(-1).after(...list);
    else rest[0].before(...list);
    say({ [over ? "front" : "back"]: list.map((item) => item.id) });
  };
  // A side of the picked and its length, across or down.
  const span = (across) =>
    across ? ["offsetLeft", "offsetWidth"] : ["offsetTop", "offsetHeight"];
  const lineUp = (list, across, places) => {
    moveTo(
      list,
      list.map((item, index) => {
        const [x, y] = at(item);
        return across
          ? [Math.round(places[index]), y]
          : [x, Math.round(places[index])];
      }),
    );
    placed(list);
  };
  const acts = {
    delete: () => {
      const gone = picked;
      if (!gone.length) return;
      pick([]);
      for (const item of gone) remove(item);
      drawn();
      say({ remove: gone.map((item) => item.id) });
    },
    duplicate: () => copy(picked),
    front: () => order(true),
    back: () => order(false),
    // By a side or the middles: left, centre or right; top, middle or bottom.
    align: (to) => {
      const across = ["left", "center", "right"].includes(to);
      const [start, length] = span(across);
      const low = Math.min(...picked.map((item) => item[start]));
      const high = Math.max(
        ...picked.map((item) => item[start] + item[length]),
      );
      const where = { center: 0.5, middle: 0.5, right: 1, bottom: 1 }[to] ?? 0;
      lineUp(
        picked,
        across,
        picked.map((item) => low + (high - low - item[length]) * where),
      );
    },
    // The first and the last stay, and the others share the room between.
    distribute: (to) => {
      const across = to === "across";
      const [start, length] = span(across);
      const list = [...picked].sort((a, b) => a[start] - b[start]);
      const [first, last] = [list[0], list.at(-1)];
      const room =
        last[start] +
        last[length] -
        first[start] -
        list.reduce((sum, item) => sum + item[length], 0);
      let next = first[start];
      lineUp(
        list,
        across,
        list.map((item) => {
          const here = next;
          next += item[length] + room / (list.length - 1);
          return here;
        }),
      );
    },
  };

  let clipped = [];
  let nudged;
  addEventListener(
    "keydown",
    (event) => {
      if (event.target.isContentEditable) return;
      const held = event.metaKey || event.ctrlKey;
      const by = {
        ArrowLeft: [-1, 0],
        ArrowRight: [1, 0],
        ArrowUp: [0, -1],
        ArrowDown: [0, 1],
      }[event.key];
      const key = held ? event.key.toLowerCase() : event.key;
      if (by && picked.length) {
        const step = event.shiftKey ? APART / 2 : 1;
        changing = true;
        moveTo(
          picked,
          picked.map((item) => [
            item.offsetLeft + by[0] * step,
            item.offsetTop + by[1] * step,
          ]),
        );
        clearTimeout(nudged);
        const list = picked;
        nudged = setTimeout(() => {
          changing = false;
          placed(list);
        }, 400);
      } else if (key === "Delete" || key === "Backspace") acts.delete();
      else if (held && key === "d") acts.duplicate();
      else if (held && key === "c") clipped = picked;
      else if (held && key === "v")
        copy(clipped.filter((item) => item.isConnected));
      else if (held && key === "a") pick(items());
      else if (key === "Escape") return pick([]);
      else return;
      // Before the keys the page forwards to the app, which would step pages.
      event.preventDefault();
    },
    true,
  );
  addEventListener("dblclick", (event) => {
    const item = event.target.closest?.("[data-at][id]:not(img)");
    // The item's own text, or that of the one heading it holds.
    const own = [...(item?.children ?? [])].filter(
      (child) => !child.hasAttribute("data-uagent"),
    );
    const field = !item
      ? undefined
      : !own.length
        ? item
        : own.length === 1 && !own[0].children.length && own[0];
    if (!field) return;
    const was = field.textContent;
    field.contentEditable = "plaintext-only";
    field.focus();
    field.addEventListener(
      "blur",
      () => {
        field.removeAttribute("contenteditable");
        changed([item]);
        if (field.textContent !== was)
          say({ set: [{ id: item.id, text: field.textContent }] });
      },
      { once: true },
    );
  });
  return { hold, pick, picked: () => picked, acts };
}
