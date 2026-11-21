// A whiteboard page drawn: its items laid where their data-at puts them,
// shapes and arrows drawn, and, in the app, changed by hand there, which the
// app writes to the file. The format: skills/artifact/references/whiteboard.md.

import { APART as COPIED, held, itemLayer, post, rectOf } from "./items.js";

const SVG = "http://www.w3.org/2000/svg";
const FILLS = {
  yellow: "#ffe680",
  orange: "#ffc48c",
  pink: "#ffbdd1",
  green: "#b4e2b8",
  blue: "#b6d3fb",
  grey: "#dedede",
};
// Shapes CSS cannot draw, as paths in a 100 by 100 box stretched to the item.
const PATHS = {
  diamond: "M50 0L100 50L50 100L0 50Z",
  hexagon: "M15 0H85L100 50L85 100H15L0 50Z",
  parallelogram: "M15 0H100L85 100H0Z",
  database: "M0 12A50 12 0 0 1 100 12V88A50 12 0 0 1 0 88Z",
  queue: "M12 0H88A12 50 0 0 1 88 100H12A12 50 0 0 1 12 0Z",
  document: "M0 0H100V86Q75 72 50 86T0 86Z",
  cloud:
    "M27 88A20 20 0 0 1 20 50A24 24 0 0 1 52 22A22 22 0 0 1 86 40A20 20 0 0 1 80 88Z",
  person: "M50 6A14 14 0 1 1 49.9 6ZM18 96Q18 48 50 48Q82 48 82 96Z",
};
// The rim that makes a cylinder of one, a line over its fill.
const RIMS = {
  database: "M0 12A50 12 0 0 0 100 12",
  queue: "M88 0A12 50 0 0 0 88 100",
};
const CSS = `
html, body { margin: 0; }
body { position: relative; min-height: 100vh; overflow: hidden; background: #fafafa;
  background-image: radial-gradient(#d4d4d4 1px, transparent 1px);
  background-size: 20px 20px; font: 16px/1.35 system-ui, sans-serif; color: #1a1a1a; }
hr[data-from] { display: none; }
[data-at] { position: absolute; box-sizing: border-box; margin: 0; z-index: 2; }
section[data-at] { z-index: 0; border: 1px solid #d0d0d0; border-radius: 12px; background: #ffffff99; }
section[data-at] > :first-child { position: absolute; left: 2px; bottom: 100%; margin: 0 0 6px;
  font-size: 15px; font-weight: 500; color: #666; white-space: nowrap; }
h1[data-at] { font-size: 34px; } h2[data-at] { font-size: 22px; }
small[data-at] { font-size: 13px; color: #555; display: block; }
.note { width: 160px; min-height: 160px; padding: 14px; background: var(--fill, ${FILLS.yellow});
  border-radius: 2px; box-shadow: 0 1px 3px #0002; overflow-wrap: anywhere; }
[data-shape] { display: flex; align-items: center; justify-content: center; padding: 8px 14px;
  text-align: center; }
[data-shape] { --fill: ${FILLS.grey}; }
[data-shape] > svg.shape { position: absolute; inset: 4px; width: calc(100% - 8px);
  height: calc(100% - 8px); z-index: -1; overflow: visible; }
[data-shape] > svg.shape path { fill: var(--fill); stroke: var(--fill); stroke-width: 8px;
  stroke-linejoin: round; vector-effect: non-scaling-stroke; }
[data-shape] > svg.shape path.rim { fill: none; stroke: #0003; stroke-width: 1.5px; }
[data-shape="rect"], [data-shape="rounded"], [data-shape="pill"], [data-shape="ellipse"] {
  background: var(--fill); }
[data-shape="rect"] { border-radius: 8px; }
[data-shape="rounded"] { border-radius: 18px; } [data-shape="pill"] { border-radius: 999px; }
[data-shape="ellipse"] { border-radius: 50%; }
[data-shape="person"] { align-items: flex-end; }
table[data-at] { border-collapse: collapse; background: #fff; }
table[data-at] :is(th, td) { border: 1px solid #b5b5b5; padding: 4px 10px; text-align: left; }
img[data-at] { object-fit: contain; background: #fff; }
a[data-at] { display: block; padding: 10px 14px; background: #fff; border: 1px solid #b5b5b5;
  color: inherit; text-decoration: none; min-width: 160px; }
a[data-at]::after { content: attr(href); display: block; font-size: 12px; color: #666; }
.widget[data-at] { overflow: hidden; }
svg.arrows { position: absolute; inset: 0; z-index: 1; pointer-events: none; overflow: visible; }
svg.arrows path { fill: none; stroke: #5c5c5c; stroke-width: 1.5px; }
svg.arrows .dash { stroke-dasharray: 6 5; }
svg.arrows text { font: 13px system-ui, sans-serif; fill: #444; paint-order: stroke;
  stroke: #fafafa; stroke-width: 6px; stroke-linejoin: round; }
svg.arrows path.hit { stroke: transparent; stroke-width: 12px; pointer-events: stroke;
  cursor: pointer; }
svg.arrows path.picked { stroke: var(--uagent-pick); }
.uagent-dot { position: absolute; width: calc(10 * var(--uagent-px));
  height: calc(10 * var(--uagent-px)); margin: calc(-5 * var(--uagent-px)); z-index: 3;
  box-sizing: border-box; border: calc(2 * var(--uagent-px)) solid var(--uagent-pick); border-radius: 50%;
  background: #fff; cursor: crosshair; touch-action: none; }
svg.uagent-drawing { position: absolute; inset: 0; z-index: 3; pointer-events: none;
  overflow: visible; }
svg.uagent-drawing path { fill: none; stroke: var(--uagent-pick); stroke-width: 1.5px;
  stroke-dasharray: 6 5; }
@media (prefers-reduced-motion: no-preference) {
  [data-uagent-glide] [data-at] { transition: left .3s, top .3s, width .3s, height .3s,
    min-height .3s, opacity .3s; }
  @starting-style { [data-uagent-glide] [data-at] { opacity: 0; } }
}
`;

const numbers = (text) =>
  (text ?? "")
    .split(/[\s,]+/)
    .filter(Boolean)
    .map(Number);
const items = () =>
  [...document.body.children].filter((element) =>
    element.hasAttribute("data-at"),
  );
const grid = (n) => Math.round(n / 10) * 10;
// An id no element has yet, made from `stem`.
function fresh(stem) {
  const kept = stem.toLowerCase().replace(/[^a-z0-9-]|\d+$/g, "");
  // An id starts with a letter.
  const base = /^[a-z]/.test(kept) ? kept : `item${kept}`;
  let n = 1;
  while (document.getElementById(`${base}${n}`)) n++;
  return `${base}${n}`;
}

// Each item where it says, at its size, filled and shaped.
function place(item) {
  const [x = 0, y = 0] = numbers(item.dataset.at);
  Object.assign(item.style, { left: `${x}px`, top: `${y}px` });
  const [w, h] = numbers(item.dataset.size);
  if (w > 0) item.style.width = `${w}px`;
  if (h > 0)
    item.style[item.matches(".note") ? "minHeight" : "height"] = `${h}px`;
  const fill = FILLS[item.dataset.fill];
  if (fill) item.style.setProperty("--fill", fill);
  const { shape } = item.dataset;
  const drawn = item.querySelector(":scope > svg.shape");
  if (drawn?.dataset.uagent === shape) return;
  drawn?.remove();
  if (!PATHS[shape]) return;
  const svg = document.createElementNS(SVG, "svg");
  svg.setAttribute("class", "shape");
  svg.dataset.uagent = shape;
  svg.setAttribute("viewBox", "0 0 100 100");
  svg.setAttribute("preserveAspectRatio", "none");
  svg.setAttribute("aria-hidden", "true");
  for (const d of [PATHS[shape], RIMS[shape]].filter(Boolean)) {
    const drawn = document.createElementNS(SVG, "path");
    drawn.setAttribute("d", d);
    if (d === RIMS[shape]) drawn.setAttribute("class", "rim");
    svg.append(drawn);
  }
  item.prepend(svg);
}

// The four sides of an item, as the way out of each.
const SIDES = [
  [0, -1],
  [1, 0],
  [0, 1],
  [-1, 0],
];
// How far an arrow runs straight out of an item, and how round it turns.
const OUT = 20;
const BEND = 10;
// How far apart two arrows stand on one side of an item.
const APART = 16;
// How far inside its edge a section is still held by it, on the screen.
const EDGE = 8;

// The arrows, each in right-angled legs from a side of one item to a side
// of the other: the route across the fewest items, then with the fewest
// turns, then the shortest. Arrows on one side of an item share it out.
function arrows(layer) {
  const boxOf = (item) =>
    item && {
      item,
      x: item.offsetLeft,
      y: item.offsetTop,
      w: item.offsetWidth,
      h: item.offsetHeight,
    };
  // A shape that meets its box only about the middle of a side.
  const curved = ({ item }) =>
    !["rect", "rounded"].includes(item.dataset.shape ?? "rect");
  const blocks = items()
    .filter((item) => !item.matches("section"))
    .map(boxOf);
  // Where an arrow meets a side of an item: on its box, or as far in as a
  // drawn shape begins there.
  const port = (box, [nx, ny], along = 0.5) => {
    const at = {
      x: nx ? box.x + (nx > 0 ? box.w : 0) : box.x + box.w * along,
      y: ny ? box.y + (ny > 0 ? box.h : 0) : box.y + box.h * along,
      nx,
      ny,
    };
    const drawn = box.item.querySelector(":scope > svg.shape path");
    const inside = () =>
      drawn.isPointInFill(
        new DOMPoint(
          ((at.x - box.x) / box.w) * 100,
          ((at.y - box.y) / box.h) * 100,
        ),
      );
    for (let step = 0; drawn && step < 40 && !inside(); step++) {
      at.x -= nx * 2;
      at.y -= ny * 2;
    }
    return at;
  };
  // A route without the points that turn nothing.
  const tidy = (points) =>
    points.reduce((kept, at) => {
      const [a, b] = kept.slice(-2);
      if (b && ((a.x === b.x && b.x === at.x) || (a.y === b.y && b.y === at.y)))
        kept.pop();
      kept.push(at);
      return kept;
    }, []);
  // The ways from one port to another: straight out of each, and across
  // beside either or half way between.
  const ways = (p, q) => {
    const a = { x: p.x + p.nx * OUT, y: p.y + p.ny * OUT };
    const b = { x: q.x + q.nx * OUT, y: q.y + q.ny * OUT };
    const [mx, my] = [(a.x + b.x) / 2, (a.y + b.y) / 2];
    return [
      [{ x: b.x, y: a.y }],
      [{ x: a.x, y: b.y }],
      [
        { x: mx, y: a.y },
        { x: mx, y: b.y },
      ],
      [
        { x: a.x, y: my },
        { x: b.x, y: my },
      ],
    ].map((across) => tidy([p, a, ...across, b, q]));
  };
  const crosses = (points, box, inset) =>
    points.some((at, index) => {
      const to = points[index + 1];
      return (
        to &&
        Math.max(at.x, to.x) > box.x + inset &&
        Math.min(at.x, to.x) < box.x + box.w - inset &&
        Math.max(at.y, to.y) > box.y + inset &&
        Math.min(at.y, to.y) < box.y + box.h - inset
      );
    });
  const length = (points) =>
    points.reduce(
      (sum, at, index) =>
        index
          ? sum +
            Math.abs(at.x - points[index - 1].x) +
            Math.abs(at.y - points[index - 1].y)
          : 0,
      0,
    );
  // A side an arrow already uses costs the next one as much as a detour.
  const used = new Map();
  const sideOf = (box, side) => `${box.item.id} ${side}`;
  const cost = (points, from, to, s, t) =>
    (crosses(points.slice(1), from, 2) || crosses(points.slice(0, -1), to, 2)
      ? 1e7
      : 0) +
    blocks.filter(
      (box) =>
        box.item !== from.item &&
        box.item !== to.item &&
        crosses(points, box, 0),
    ).length *
      1e4 +
    ((used.get(sideOf(from, s)) ?? 0) + (used.get(sideOf(to, t)) ?? 0)) * 200 +
    points.length * 80 +
    length(points);
  const best = (from, to, sides, along = [0.5, 0.5]) => {
    let found;
    for (const [s, t] of sides)
      for (const points of ways(
        port(from, SIDES[s], along[0]),
        port(to, SIDES[t], along[1]),
      )) {
        const costs = cost(points, from, to, s, t);
        if (!found || costs < found.costs) found = { points, costs, s, t };
      }
    return found;
  };
  const every = SIDES.flatMap((_, s) => SIDES.map((_, t) => [s, t]));
  const lines = [];
  for (const line of document.querySelectorAll("hr[data-from][data-to]")) {
    const from = boxOf(document.getElementById(line.dataset.from));
    const to = boxOf(document.getElementById(line.dataset.to));
    if (!from || !to) continue;
    const one = { line, from, to, ...best(from, to, every) };
    for (const key of [sideOf(from, one.s), sideOf(to, one.t)])
      used.set(key, (used.get(key) ?? 0) + 1);
    lines.push(one);
  }
  used.clear();
  // Where along its side each end of an arrow stands. Two items that face
  // each other are joined straight, in the middle of what they share; any
  // other end takes the middle of its side, and ends on one side keep apart.
  const span = (box, side) =>
    SIDES[side][0] ? [box.y, box.y + box.h] : [box.x, box.x + box.w];
  const taken = new Map();
  const stand = (box, side, wish) => {
    const [low, high] = span(box, side);
    const edge = curved(box) ? (high - low) / 4 : 8;
    const others = taken.get(sideOf(box, side)) ?? [];
    let at = wish;
    for (
      let step = 1;
      others.some((other) => Math.abs(other - at) < APART);
      step++
    )
      at = wish + (step % 2 ? 1 : -1) * Math.ceil(step / 2) * APART;
    taken.set(sideOf(box, side), [...others, at]);
    return (
      (Math.min(high - edge, Math.max(low + edge, at)) - low) / (high - low)
    );
  };
  const shared = (one) => {
    const [a, b] = [span(one.from, one.s), span(one.to, one.t)];
    const [low, high] = [Math.max(a[0], b[0]), Math.min(a[1], b[1])];
    return Math.abs(one.s - one.t) === 2 && low < high && (low + high) / 2;
  };
  // The straight ones first: they have one place, the others any.
  for (const one of [...lines].sort((a, b) => !shared(a) - !shared(b))) {
    const middle = ([from, to]) => (from + to) / 2;
    one.along = [
      stand(one.from, one.s, shared(one) || middle(span(one.from, one.s))),
      stand(one.to, one.t, shared(one) || middle(span(one.to, one.t))),
    ];
  }
  layer.replaceChildren(defs());
  for (const one of lines) {
    const { line } = one;
    const { points } = best(one.from, one.to, [[one.s, one.t]], one.along);
    const far = (a, b) => Math.abs(a.x - b.x) + Math.abs(a.y - b.y);
    const path = document.createElementNS(SVG, "path");
    path.setAttribute(
      "d",
      points
        .map((at, index) => {
          const [was, next] = [points[index - 1], points[index + 1]];
          if (!was) return `M${at.x} ${at.y}`;
          if (!next) return `L${at.x} ${at.y}`;
          // A turn starts and ends this far from its corner.
          const round = Math.min(BEND, far(was, at) / 2, far(at, next) / 2);
          const near = (to) => [
            at.x + Math.sign(to.x - at.x) * round,
            at.y + Math.sign(to.y - at.y) * round,
          ];
          return `L${near(was)}Q${at.x} ${at.y} ${near(next)}`;
        })
        .join(""),
    );
    const head = line.dataset.head ?? "end";
    if (head !== "none") path.setAttribute("marker-end", "url(#uagent-head)");
    if (head === "both") path.setAttribute("marker-start", "url(#uagent-tail)");
    if (line.hasAttribute("data-dash")) path.setAttribute("class", "dash");
    if (line === chosen) path.classList.add("picked");
    // Wider than its line, for a press to pick it.
    const hit = document.createElementNS(SVG, "path");
    hit.setAttribute("d", path.getAttribute("d"));
    hit.setAttribute("class", "hit");
    hitOf.set(hit, line);
    layer.append(path, hit);
    if (line.dataset.label) {
      // Beside the middle of a leg: over a level one, right of an
      // upright one, on the longest that leaves the label clear of items.
      const wide = line.dataset.label.length * 7;
      const where = ([a, b]) => {
        const level = a.y === b.y;
        const [x, y] = [(a.x + b.x) / 2, (a.y + b.y) / 2];
        return {
          level,
          x: x + (level ? 0 : 8),
          y: y - (level ? 8 : 0),
          box: level
            ? { x: x - wide / 2, y: y - 24, w: wide, h: 20 }
            : { x: x + 8, y: y - 10, w: wide, h: 20 },
        };
      };
      const legs = points
        .slice(1)
        .map((to, index) => [points[index], to])
        .sort((p, q) => far(...q) - far(...p))
        .map(where);
      const clear = ({ box }) =>
        !blocks.some(
          (other) =>
            box.x < other.x + other.w &&
            other.x < box.x + box.w &&
            box.y < other.y + other.h &&
            other.y < box.y + box.h,
        );
      const { level, x, y } = legs.find(clear) ?? legs[0];
      const text = document.createElementNS(SVG, "text");
      text.setAttribute("x", `${x}`);
      text.setAttribute("y", `${y}`);
      text.setAttribute("text-anchor", level ? "middle" : "start");
      if (!level) text.setAttribute("dominant-baseline", "middle");
      text.textContent = line.dataset.label;
      layer.append(text);
    }
  }
}
function defs() {
  const made = document.createElementNS(SVG, "defs");
  for (const [id, d] of [
    ["uagent-head", "M0 0L8 4L0 8Z"],
    ["uagent-tail", "M8 0L0 4L8 8Z"],
  ]) {
    const marker = document.createElementNS(SVG, "marker");
    for (const [name, value] of Object.entries({
      id,
      viewBox: "0 0 8 8",
      refX: id === "uagent-head" ? 8 : 0,
      refY: 4,
      markerWidth: 8,
      markerHeight: 8,
      markerUnits: "userSpaceOnUse",
      orient: "auto",
    }))
      marker.setAttribute(name, `${value}`);
    const tip = document.createElementNS(SVG, "path");
    tip.setAttribute("d", d);
    tip.style.fill = "#5c5c5c";
    tip.style.stroke = "none";
    marker.append(tip);
    made.append(marker);
  }
  return made;
}

// The arrow picked, and the arrow each wide line under one stands for.
let chosen;
const hitOf = new WeakMap();
// What each of the app's tools puts where the board is next pressed; any
// other tool is a shape of that kind.
const MADE = {
  note: { tag: "p", note: true, fill: "yellow", text: "" },
  text: { tag: "p", text: "Text" },
  section: { tag: "section", size: [400, 240], text: "Section" },
};
const TEXT = "h1, h2, p:not(.note), small";

// In the app the board is changed by hand (items.js): the app's tools put a
// note, a text, a section or a shape where the board is next pressed, an
// arrow is drawn from a dot on the picked item's side (or with the arrow
// tool from any item) to another, and the app's bar changes what is picked.
// Each change is said to the app, which writes it and undoes it with ⌘Z.
function edits(layer, signal) {
  const say = (change) => post({ type: "change", ...change });
  const lay = (item) => {
    place(item);
    sized.observe(item);
  };
  // A section is held by its name or its edge: a press inside it is one on
  // the board under it.
  const framed = (item) => item.localName === "section";
  const holds = (item, press) => {
    if (item !== press.target || !framed(item)) return true;
    const edge =
      EDGE /
      (Number(
        document.documentElement.style.getPropertyValue("--uagent-scale"),
      ) || 1);
    const { left, top, right, bottom } = item.getBoundingClientRect();
    const { clientX: x, clientY: y } = press;
    return Math.min(x - left, right - x, y - top, bottom - y) <= edge;
  };
  const itemOf = (target, press) => {
    const item = target.closest?.("[data-at]");
    return item?.id && (!press || holds(item, press)) ? item : undefined;
  };
  const dots = SIDES.map(() => {
    const dot = document.createElement("span");
    dot.className = "uagent-dot";
    dot.dataset.uagent = "";
    return dot;
  });
  // What is picked, said with where it is: items, or an arrow.
  const show = (picked) => {
    if (picked.length && chosen) pickLine(undefined);
    const [one, more] = picked;
    dots.forEach((dot, index) => {
      if (!one || more) return dot.remove();
      const [nx, ny] = SIDES[index];
      dot.style.left = `${one.offsetLeft + (one.offsetWidth * (1 + nx)) / 2}px`;
      dot.style.top = `${one.offsetTop + (one.offsetHeight * (1 + ny)) / 2}px`;
      if (!dot.isConnected) document.body.append(dot);
    });
    post({
      type: "items",
      items: picked.map((item) => ({
        selector: `#${globalThis.CSS.escape(item.id)}`,
        tag: item.localName,
        text: item.textContent.trim().slice(0, 60),
        shape: item.dataset.shape,
        fill: item.dataset.fill,
        note: item.matches(".note"),
      })),
      arrow: !!chosen,
      rect: rectOf(
        (chosen ? [layer.querySelector("path.picked")] : picked).filter(
          Boolean,
        ),
      ),
    });
  };
  const at = (item) => [item.offsetLeft, item.offsetTop];
  const removeItem = (item) => {
    for (const line of document.querySelectorAll("hr[data-from][data-to]"))
      if (line.dataset.from === item.id || line.dataset.to === item.id)
        line.remove();
    item.remove();
  };
  // A new item where the board was pressed, picked.
  const insert = (tool, event) => {
    const add = {
      id: fresh(MADE[tool] ? tool : "shape"),
      at: [grid(event.clientX), grid(event.clientY)],
      ...(MADE[tool] ?? { tag: "div", shape: tool, size: [160, 80], text: "" }),
    };
    const item = document.createElement(add.tag);
    item.id = add.id;
    if (add.note) item.className = "note";
    if (add.shape) item.dataset.shape = add.shape;
    if (add.fill) item.dataset.fill = add.fill;
    item.dataset.at = add.at.join(" ");
    if (add.size) item.dataset.size = add.size.join(" ");
    if (add.tag === "section") item.append(document.createElement("h2"));
    (item.firstChild ?? item).textContent = add.text;
    const last = items().at(-1);
    if (last) last.after(item);
    else document.body.append(item);
    lay(item);
    arrows(layer);
    hand.pick([item]);
    say({ add });
  };
  const hand = itemLayer({
    touch: "[data-at]:not(section), section[data-at] > :first-child",
    items: () => items().filter((item) => item.id),
    itemOf,
    holds,
    whole: framed,
    box: (item) => [...at(item), item.offsetWidth, item.offsetHeight],
    put: (item, [x, y, w, h], resized) => {
      item.dataset.at = `${x} ${y}`;
      if (resized) item.dataset.size = `${w} ${h}`;
      lay(item);
    },
    tall: () => true,
    snap: grid,
    said: (list, resized) => ({
      set: list.map((item) => ({
        id: item.id,
        at: at(item),
        ...(resized && { size: numbers(item.dataset.size) }),
      })),
    }),
    copy: (list) => {
      const made = list.map((item) => {
        const copied = item.cloneNode(true);
        copied.removeAttribute("data-uagent-picked");
        copied.removeAttribute("data-uagent-changed");
        // An id names one element: what the copy holds keeps none.
        for (const inner of copied.querySelectorAll("[id]"))
          inner.removeAttribute("id");
        copied.id = fresh(item.id);
        const [x, y] = at(item);
        copied.dataset.at = `${x + COPIED} ${y + COPIED}`;
        items().at(-1).after(copied);
        lay(copied);
        return copied;
      });
      // The arrows between two of them, between their copies.
      const as = new Map(list.map((item, index) => [item.id, made[index].id]));
      for (const line of document.querySelectorAll("hr[data-from][data-to]")) {
        const { from, to } = line.dataset;
        if (!as.has(from) || !as.has(to)) continue;
        const copied = line.cloneNode();
        copied.dataset.from = as.get(from);
        copied.dataset.to = as.get(to);
        [...document.querySelectorAll("hr[data-from]")].at(-1).after(copied);
      }
      return [
        made,
        {
          copy: list.map((item, index) => ({
            id: item.id,
            as: made[index].id,
            at: numbers(made[index].dataset.at),
          })),
        },
      ];
    },
    // The picked laid over the rest, or under them.
    order: (picked, over) => {
      const list = items().filter((item) => picked.includes(item));
      const rest = items().filter((item) => !picked.includes(item));
      if (!list.length || !rest.length) return;
      if (over) rest.at(-1).after(...list);
      else rest[0].before(...list);
      return { [over ? "front" : "back"]: list.map((item) => item.id) };
    },
    remove: (list) => {
      for (const item of list) removeItem(item);
      return { remove: list.map((item) => item.id) };
    },
    // The item's own text, or that of the one element (a section's name)
    // that holds all of it.
    retype: (item) => {
      const own = [...item.children].filter(
        (child) => !child.hasAttribute("data-uagent"),
      );
      const field = item.matches("img")
        ? undefined
        : !own.length
          ? item
          : own.length === 1 &&
            !own[0].children.length &&
            own[0].textContent.trim() === item.textContent.trim() &&
            own[0];
      if (!field) return;
      const was = field.textContent;
      field.contentEditable = "plaintext-only";
      field.focus();
      field.addEventListener(
        "blur",
        () => {
          field.removeAttribute("contenteditable");
          lay(item);
          arrows(layer);
          hand.hold();
          if (field.textContent !== was)
            say({ set: [{ id: item.id, text: field.textContent }] });
        },
        { once: true },
      );
    },
    insert: (tool, event) => {
      if (tool !== "arrow") {
        insert(tool, event);
        return true;
      }
      const from = itemOf(event.target);
      if (from) connect(from, event, tool);
      return !!from;
    },
    act: (act, value) => {
      const [item] = hand.picked();
      if (act === "delete" && chosen) unline();
      else if ((act === "shape" || act === "fill") && item) {
        item.dataset[act] = value;
        lay(item);
        arrows(layer);
        hand.hold();
        say({ set: [{ id: item.id, [act]: value }] });
      } else if (act === "tag" && item?.matches(TEXT)) {
        const made = document.createElement(value);
        for (const attribute of item.attributes)
          made.setAttribute(attribute.name, attribute.value);
        made.append(...item.childNodes);
        item.replaceWith(made);
        lay(made);
        hand.pick([made]);
        say({ set: [{ id: made.id, tag: value }] });
      } else return false;
      return true;
    },
    drawn: () => arrows(layer),
    show,
    say,
  });
  const pickLine = (line) => {
    chosen = line;
    arrows(layer);
    if (line) hand.pick([]);
  };
  const unline = () => {
    const { from, to } = chosen.dataset;
    chosen.remove();
    pickLine(undefined);
    hand.pick([]);
    say({ unarrow: { from, to } });
  };

  // An arrow from the item to the one the drag ends on, a line from where
  // it started until then; the arrow tool that drew none stays armed.
  const connect = (from, event, tool) => {
    const drawing = document.createElementNS(SVG, "svg");
    drawing.setAttribute("class", "uagent-drawing");
    drawing.dataset.uagent = "";
    const path = document.createElementNS(SVG, "path");
    drawing.append(path);
    document.body.append(drawing);
    const [x, y] = [event.clientX, event.clientY];
    held(
      event,
      (next) =>
        path.setAttribute("d", `M${x} ${y}L${next.clientX} ${next.clientY}`),
      (up) => {
        drawing.remove();
        const to =
          up && itemOf(document.elementFromPoint(up.clientX, up.clientY));
        if (!to || to === from) return tool && hand.arm(tool);
        const line = document.createElement("hr");
        line.dataset.from = from.id;
        line.dataset.to = to.id;
        (
          [...document.querySelectorAll("hr[data-from]")].at(-1) ??
          items().at(-1)
        ).after(line);
        arrows(layer);
        say({ arrow: { from: from.id, to: to.id } });
      },
      signal,
    );
  };
  addEventListener(
    "pointerdown",
    (event) => {
      if (event.button) return;
      const dot = dots.indexOf(event.target);
      const hit = hitOf.get(event.target);
      if (dot >= 0) connect(hand.picked()[0], event);
      else if (hit) pickLine(hit);
      else {
        if (chosen) pickLine(undefined);
        return;
      }
      // The press is the board's, not the items'.
      event.stopPropagation();
      event.preventDefault();
      focus();
    },
    { capture: true, signal },
  );
  addEventListener(
    "keydown",
    (event) => {
      if (event.target.isContentEditable || !chosen) return;
      if (event.key === "Escape") pickLine(undefined);
      if (event.key === "Delete" || event.key === "Backspace") {
        event.preventDefault();
        unline();
      }
    },
    { capture: true, signal },
  );
  return hand;
}

let layer;
let sized;
let hand;
let off;
function draw() {
  const style = document.createElement("style");
  style.textContent = CSS;
  document.head.prepend(style);
  for (const item of items()) place(item);
  layer = document.createElementNS(SVG, "svg");
  layer.setAttribute("class", "arrows");
  layer.setAttribute("aria-hidden", "true");
  layer.dataset.uagent = "";
  document.body.prepend(layer);
  arrows(layer);
  // Fonts and pictures change the items' sizes: the arrows follow.
  sized = new ResizeObserver(() => arrows(layer));
  for (const item of items()) sized.observe(item);
  return [];
}

// Edit on: the board is changed by hand; off, it is only looked at.
export function edit(on) {
  if (!on) {
    hand?.end();
    off?.abort();
    hand = undefined;
    chosen = undefined;
    return arrows(layer);
  }
  if (hand) return hand.hold();
  off = new AbortController();
  hand = edits(layer, off.signal);
}

// The file changed under the page: each item glides to where it now says,
// a new one fades in, and the arrows and the picked items' handles follow.
export function redraw() {
  document.documentElement.toggleAttribute("data-uagent-glide", true);
  for (const item of items()) {
    place(item);
    sized.observe(item);
  }
  const step = () => {
    arrows(layer);
    hand?.hold();
    if (items().some((item) => item.getAnimations().length))
      requestAnimationFrame(step);
    else document.documentElement.removeAttribute("data-uagent-glide");
  };
  step();
}

// Whether a person is changing an item: holding, nudging or retyping it.
export const busy = () => !!hand?.busy();

export const ready =
  document.readyState === "loading"
    ? new Promise((done) =>
        addEventListener("DOMContentLoaded", () => done(draw())),
      )
    : Promise.resolve(draw());
