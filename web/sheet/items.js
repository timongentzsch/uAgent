// Items of a page changed by hand in the app: a whiteboard's (whiteboard.js)
// or a slide's (slide.js), which say what the items are and how one is
// placed. A press picks one, with Shift adds or drops it; a drag from the
// empty page frames the items it touches (on a touch screen it scrolls); a
// drag moves the picked together and a corner resizes one; a press again on
// the one picked types in it; the keys nudge, delete, duplicate, copy and
// paste, pick all and let go. The app arms a tool, which the next press
// puts, and asks acts of what is picked. Each change is said once, for the
// app to write.

// Drawn the same size on screen at any zoom of the page (--uagent-scale).
const CSS = `
:root { --uagent-px: calc(1px / var(--uagent-scale, 1)); }
[data-uagent-picked] { outline: calc(2 * var(--uagent-px)) solid #1a56db;
  outline-offset: calc(2 * var(--uagent-px)); touch-action: none; }
.uagent-handle { position: absolute; width: calc(10 * var(--uagent-px));
  height: calc(10 * var(--uagent-px)); margin: calc(-5 * var(--uagent-px));
  z-index: 3; background: #1a56db; touch-action: none; }
.uagent-frame { position: absolute; z-index: 3;
  border: var(--uagent-px) solid #1a56db; background: #1a56db1a; }
@media (pointer: coarse) {
  .uagent-handle::after { content: ""; position: absolute;
    inset: calc(-17 * var(--uagent-px)); }
}
`;
// The corners, as the sides of the item each one moves.
const CORNERS = [
  [-1, -1],
  [1, -1],
  [-1, 1],
  [1, 1],
];
const ARROWS = {
  ArrowLeft: [-1, 0],
  ArrowRight: [1, 0],
  ArrowUp: [0, -1],
  ArrowDown: [0, 1],
};
// How far a copy lies from what it copies, and the smallest an item gets.
export const APART = 20;
const LEAST = 20;

// The box of elements together, in the window.
export function rectOf(list) {
  const rects = list.map((one) => one.getBoundingClientRect());
  if (!rects.length) return undefined;
  const [left, top] = ["left", "top"].map((side) =>
    Math.min(...rects.map((rect) => rect[side])),
  );
  const [right, bottom] = ["right", "bottom"].map((side) =>
    Math.max(...rects.map((rect) => rect[side])),
  );
  return { x: left, y: top, width: right - left, height: bottom - top };
}

// The type says: `items` the page has, `itemOf` the one a press on an
// element picks, `holds` whether a press on a picked item is on it at all,
// `whole` whether a frame picks an item only when it holds all of it, `box` an item's [x, y, w, h] in the page, `put` it there
// (`sized`: at that size), `tall` whether a corner sets its height, `snap` a
// length as it is kept; `said` the change that says items' places (and
// sizes), `copy` copies them and `order` lays them over or under the rest,
// `remove` takes them away, each giving the change; `retype` types in an
// item at a press, `insert` puts what a tool makes where a press is (or
// does not take the press), `act` does what only the type does, `drawn`
// follows a change, `show` says what is picked, `say` tells the app; on
// `touch`, a finger drags rather than scrolls.
export function itemLayer(type) {
  const { box, put, say: tell, snap = Math.round } = type;
  const off = new AbortController();
  const listen = (kind, handle, capture) =>
    addEventListener(kind, handle, { capture, signal: off.signal });
  const style = document.createElement("style");
  style.textContent = `${CSS}:is(${type.touch}) { touch-action: none; }`;
  document.head.append(style);
  let picked = [];
  // Whether a person is changing an item: holding or nudging it.
  let changing = false;
  // The items the arrow keys move, their places not yet said: said once the
  // key is up, and before anything else changes the page.
  let owed;
  const say = (change) => {
    if (owed) {
      changing = false;
      tell(type.said(owed));
      owed = undefined;
    }
    if (change) tell(change);
  };
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
      const [x, y, w, h] = box(one);
      const [sx, sy] = CORNERS[index];
      handle.style.left = `${x + (sx > 0 ? w : 0)}px`;
      handle.style.top = `${y + (sy > 0 ? h : 0)}px`;
      if (!handle.isConnected) document.documentElement.append(handle);
    });
    type.show(picked);
  };
  const pick = (next) => {
    for (const item of picked) item.removeAttribute("data-uagent-picked");
    picked = next;
    for (const item of picked) item.setAttribute("data-uagent-picked", "");
    hold();
  };
  const changed = () => {
    type.drawn?.();
    hold();
  };
  const placed = (list) => say(type.said(list));
  const moveTo = (list, places) => {
    list.forEach((item, index) => {
      const [, , w, h] = box(item);
      put(item, [...places[index], w, h]);
    });
    changed();
  };

  // What a drag does as it goes, and once it is let go.
  const move = (list) => {
    const starts = list.map(box);
    const [x, y] = starts[0];
    return [
      (dx, dy) =>
        moveTo(
          list,
          starts.map(([sx, sy]) => [
            sx + snap(x + dx) - x,
            sy + snap(y + dy) - y,
          ]),
        ),
      () => placed(list),
    ];
  };
  const resize = (item, [sx, sy]) => {
    const [x, y, w, h] = box(item);
    const tall = type.tall(item);
    // A span with its start (side < 0) or its end moved by d.
    const side = (start, size, toward, d) => {
      if (toward > 0) return [start, Math.max(LEAST, snap(size + d))];
      const moved = Math.min(snap(start + d), start + size - LEAST);
      return [moved, start + size - moved];
    };
    return [
      (dx, dy) => {
        const [left, width] = side(x, w, sx, dx);
        const [top, height] = tall ? side(y, h, sy, dy) : [y, h];
        put(item, [left, top, width, height], true);
        changed();
      },
      () => say(type.said([item], true)),
    ];
  };
  const frame = ([x, y], adding) => {
    const was = adding ? picked : [];
    let shown;
    return [
      (dx, dy) => {
        const [left, top] = [Math.min(x, x + dx), Math.min(y, y + dy)];
        const [width, height] = [Math.abs(dx), Math.abs(dy)];
        if (!shown) {
          shown = document.createElement("div");
          shown.className = "uagent-frame";
          shown.dataset.uagent = "";
          document.documentElement.append(shown);
        }
        Object.assign(shown.style, {
          left: `${left + scrollX}px`,
          top: `${top + scrollY}px`,
          width: `${width}px`,
          height: `${height}px`,
        });
        const [x0, y0] = [left + scrollX, top + scrollY];
        const touched = type.items().filter((item) => {
          const [ix, iy, iw, ih] = box(item);
          if (was.includes(item)) return false;
          return type.whole?.(item)
            ? x0 <= ix &&
                ix + iw <= x0 + width &&
                y0 <= iy &&
                iy + ih <= y0 + height
            : ix < x0 + width &&
                x0 < ix + iw &&
                iy < y0 + height &&
                y0 < iy + ih;
        });
        pick([...was, ...touched]);
      },
      () => shown?.remove(),
    ];
  };
  // A tool the app armed, with what it makes: the next press is its own.
  let armed = "";
  let made;
  const arm = (tool, making) => {
    armed = tool;
    made = making;
    document.documentElement.style.cursor = tool ? "crosshair" : "";
  };
  listen(
    "pointerdown",
    (event) => {
      say();
      if (event.button || !armed || !type.insert(armed, event, made)) return;
      arm("");
      event.stopImmediatePropagation();
      event.preventDefault();
      focus();
    },
    true,
  );
  // Two fingers zoom the stage, as the app does for a pinch it sees.
  const pinch = (fingers) => {
    const done = new AbortController();
    const spread = () => {
      const [a, b] = [...fingers.values()];
      return [Math.hypot(a.screenX - b.screenX, a.screenY - b.screenY), a, b];
    };
    let [was] = spread();
    const follow = (next) => {
      if (!fingers.has(next.pointerId)) return;
      fingers.set(next.pointerId, next);
      const [now, a, b] = spread();
      if (was && now)
        parent.postMessage(
          {
            uagentDesign: 1,
            type: "zoom",
            by: now / was,
            x: (a.clientX + b.clientX) / 2,
            y: (a.clientY + b.clientY) / 2,
          },
          "*",
        );
      was = now;
    };
    const signal = AbortSignal.any([done.signal, off.signal]);
    addEventListener("pointermove", follow, { signal });
    for (const kind of ["pointerup", "pointercancel"])
      addEventListener(
        kind,
        (up) => fingers.has(up.pointerId) && done.abort(),
        { signal },
      );
  };
  // The pointer of the drag going on, and what a second finger does to it.
  let pressing;
  let joined;
  listen("pointerdown", (event) => {
    if (pressing !== undefined && event.pointerType === "touch")
      return joined?.(event);
    if (event.button || pressing !== undefined) return;
    if (event.target.isContentEditable) return;
    // A press elsewhere ends the typing in an item, which saves it.
    if (document.activeElement?.isContentEditable)
      document.activeElement.blur();
    const corner = handles.indexOf(event.target);
    const item =
      picked.find(
        (one) =>
          one.contains(event.target) && type.holds?.(one, event) !== false,
      ) ?? type.itemOf(event.target, event);
    const start = [event.clientX, event.clientY];
    let drag;
    let again;
    if (corner >= 0) drag = resize(picked[0], CORNERS[corner]);
    else if (item && event.shiftKey)
      pick(
        picked.includes(item)
          ? picked.filter((other) => other !== item)
          : [...picked, item],
      );
    else if (item) {
      again = picked.length === 1 && picked[0] === item;
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
    pressing = event.pointerId;
    let moved = false;
    let last = event;
    const done = new AbortController();
    // The items' attributes as the drag found them.
    const kept = picked.map((one) => [
      one,
      [...one.attributes].map(({ name, value }) => [name, value]),
    ]);
    // A second finger: the items go back, nothing is said, and the two zoom.
    joined = (second) => {
      done.abort();
      pressing = undefined;
      changing = false;
      for (const [one, attributes] of kept) {
        for (const { name } of [...one.attributes]) one.removeAttribute(name);
        for (const [name, value] of attributes) one.setAttribute(name, value);
      }
      changed();
      pinch(
        new Map([
          [last.pointerId, last],
          [second.pointerId, second],
        ]),
      );
    };
    const follow = (next) => {
      if (next.pointerId !== pressing) return;
      last = next;
      const [dx, dy] = [next.clientX - start[0], next.clientY - start[1]];
      moved ||= Math.hypot(dx, dy) > 3;
      if (moved) drag[0](dx, dy);
    };
    const drop = (up) => {
      if (up.pointerId !== pressing) return;
      done.abort();
      pressing = undefined;
      changing = false;
      if (moved) drag[1]();
      else if (again && up.type === "pointerup") type.retype(item, up);
    };
    const signal = AbortSignal.any([done.signal, off.signal]);
    addEventListener("pointermove", follow, { signal });
    addEventListener("pointerup", drop, { signal });
    addEventListener("pointercancel", drop, { signal });
  });

  // Copies of the items, over the rest, picked.
  const copy = (list) => {
    if (!list.length) return;
    const [made, change] = type.copy(list);
    say(change);
    changed();
    pick(made);
  };
  const order = (over) => {
    const change = type.order(picked, over);
    if (change) say(change);
  };
  // A side of the picked and its length, across or down, in their boxes.
  const span = (across) => (across ? [0, 2] : [1, 3]);
  const lineUp = (list, across, places) => {
    moveTo(
      list,
      list.map((item, index) => {
        const [x, y] = box(item);
        return across
          ? [Math.round(places[index]), y]
          : [x, Math.round(places[index])];
      }),
    );
    placed(list);
  };
  const acts = {
    delete: () => {
      say();
      const gone = picked;
      if (!gone.length) return;
      pick([]);
      say(type.remove(gone));
      type.drawn?.();
    },
    duplicate: () => copy(picked),
    front: () => order(true),
    back: () => order(false),
    // By a side or the middles: left, centre or right; top, middle or bottom.
    align: (to) => {
      const across = ["left", "center", "right"].includes(to);
      const [start, length] = span(across);
      const boxes = picked.map(box);
      const low = Math.min(...boxes.map((one) => one[start]));
      const high = Math.max(...boxes.map((one) => one[start] + one[length]));
      const where = { center: 0.5, middle: 0.5, right: 1, bottom: 1 }[to] ?? 0;
      lineUp(
        picked,
        across,
        boxes.map((one) => low + (high - low - one[length]) * where),
      );
    },
    // The first and the last stay, and the others share the room between.
    distribute: (to) => {
      const across = to === "across";
      const [start, length] = span(across);
      const list = picked
        .map((item) => [item, box(item)])
        .sort((a, b) => a[1][start] - b[1][start]);
      const [first, last] = [list[0][1], list.at(-1)[1]];
      const room =
        last[start] +
        last[length] -
        first[start] -
        list.reduce((sum, [, one]) => sum + one[length], 0);
      let next = first[start];
      lineUp(
        list.map(([item]) => item),
        across,
        list.map(([, one]) => {
          const here = next;
          next += one[length] + room / (list.length - 1);
          return here;
        }),
      );
    },
  };

  let clipped = [];
  listen(
    "keydown",
    (event) => {
      if (event.target.isContentEditable) return;
      const held = event.metaKey || event.ctrlKey;
      const by = ARROWS[event.key];
      const key = held ? event.key.toLowerCase() : event.key;
      if (!by || owed !== picked) say();
      if (by && picked.length) {
        const step = event.shiftKey ? APART / 2 : 1;
        changing = true;
        moveTo(
          picked,
          picked.map((item) => {
            const [x, y] = box(item);
            return [x + by[0] * step, y + by[1] * step];
          }),
        );
        owed = picked;
      } else if (key === "Delete" || key === "Backspace") acts.delete();
      else if (held && key === "d") acts.duplicate();
      else if (held && key === "c") clipped = picked;
      else if (held && key === "v")
        copy(clipped.filter((item) => item.isConnected));
      else if (held && key === "a") pick(type.items());
      else if (key === "Escape") {
        arm("");
        return pick([]);
      } else return;
      // Before the keys the page forwards to the app, which would step pages.
      event.preventDefault();
    },
    true,
  );
  listen("keyup", (event) => ARROWS[event.key] && say());
  // What the app asks: a tool armed, or an act on what is picked.
  listen("message", (event) => {
    const { data } = event;
    if (event.source !== parent || data?.uagentDesign !== 1) return;
    say();
    if (data.type === "arm") arm(String(data.tool), data.made);
    if (data.type !== "act" || type.act?.(data.act, data.value)) return;
    if (Object.hasOwn(acts, data.act)) acts[data.act](data.value);
  });
  // Said as the layer starts: the app shows its tools once the page takes
  // presses.
  hold();
  return {
    arm,
    hold,
    pick,
    picked: () => picked,
    busy: () => changing || !!document.activeElement?.isContentEditable,
    // The layer gone: nothing picked, nothing listened to.
    end: () => {
      say();
      pick([]);
      off.abort();
      style.remove();
      arm("");
    },
  };
}
