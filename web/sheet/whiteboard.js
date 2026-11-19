// A whiteboard page drawn: its items laid where their data-at puts them,
// shapes and arrows drawn, and, opened alone in the app, items moved,
// resized and retyped there, which the app writes to the file. The format:
// skills/artifact/references/whiteboard.md.

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
[data-uagent-picked] { outline: 2px solid #1a56db; outline-offset: 2px; }
.uagent-handle { position: absolute; width: 10px; height: 10px; z-index: 3;
  background: #1a56db; cursor: nwse-resize; }
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
  if (!PATHS[shape] || item.querySelector(":scope > svg.shape")) return;
  const svg = document.createElementNS(SVG, "svg");
  svg.setAttribute("class", "shape");
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
    layer.append(path);
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

// Opened alone in the app: a press picks an item and a drag moves it, its
// corner resizes it, the arrows move it a pixel (with Shift ten), a double
// click retypes a note or a text. Each change is said to the app, which
// writes it, and undoes it with ⌘Z.
function edits(layer) {
  const say = (change) =>
    parent.postMessage({ uagentDesign: 1, type: "board", ...change }, "*");
  const grid = (n) => Math.round(n / 10) * 10;
  let picked;
  const handle = document.createElement("span");
  handle.className = "uagent-handle";
  const pick = (item) => {
    picked?.removeAttribute("data-uagent-picked");
    picked = item;
    if (!item) return handle.remove();
    item.setAttribute("data-uagent-picked", "");
    document.body.append(handle);
    hold();
  };
  const hold = () => {
    if (!picked) return;
    handle.style.left = `${picked.offsetLeft + picked.offsetWidth - 5}px`;
    handle.style.top = `${picked.offsetTop + picked.offsetHeight - 5}px`;
  };
  const at = (item) => [item.offsetLeft, item.offsetTop];
  const moveTo = (item, x, y) => {
    item.dataset.at = `${x} ${y}`;
    place(item);
    arrows(layer);
    hold();
  };
  addEventListener("pointerdown", (event) => {
    if (event.button || event.target.isContentEditable) return;
    // A press elsewhere ends the typing in an item, which saves it.
    if (document.activeElement?.isContentEditable)
      document.activeElement.blur();
    const resizing = event.target === handle;
    const item = resizing ? picked : event.target.closest?.("[data-at]");
    pick(item);
    if (!item?.id) return;
    const [x, y, w, h] = [...at(item), item.offsetWidth, item.offsetHeight];
    const start = [event.clientX, event.clientY];
    let moved = false;
    const drag = (next) => {
      const [dx, dy] = [next.clientX - start[0], next.clientY - start[1]];
      moved ||= Math.hypot(dx, dy) > 3;
      if (!moved) return;
      if (!resizing) return moveTo(item, grid(x + dx), grid(y + dy));
      item.dataset.size = `${Math.max(20, grid(w + dx))} ${Math.max(20, grid(h + dy))}`;
      place(item);
      arrows(layer);
      hold();
    };
    const drop = () => {
      removeEventListener("pointermove", drag);
      if (!moved) return;
      say(
        resizing
          ? { id: item.id, size: numbers(item.dataset.size) }
          : { id: item.id, at: at(item) },
      );
    };
    addEventListener("pointermove", drag);
    addEventListener("pointerup", drop, { once: true });
    // No text is selected by a drag; the page still takes the keys.
    event.preventDefault();
    focus();
  });
  let nudged;
  addEventListener(
    "keydown",
    (event) => {
      if (event.target.isContentEditable) return;
      const by = {
        ArrowLeft: [-1, 0],
        ArrowRight: [1, 0],
        ArrowUp: [0, -1],
        ArrowDown: [0, 1],
      }[event.key];
      if (!picked?.id || !by) return;
      event.preventDefault();
      const step = event.shiftKey ? 10 : 1;
      const [x, y] = at(picked);
      moveTo(picked, x + by[0] * step, y + by[1] * step);
      clearTimeout(nudged);
      const item = picked;
      nudged = setTimeout(() => say({ id: item.id, at: at(item) }), 400);
      // Before the keys the page forwards to the app, which would step pages.
    },
    true,
  );
  addEventListener("dblclick", (event) => {
    const item = event.target.closest?.(
      ".note[id], h1[id], h2[id], p[id], small[id]",
    );
    if (!item || item.children.length) return;
    const was = item.textContent;
    item.contentEditable = "plaintext-only";
    item.focus();
    item.addEventListener(
      "blur",
      () => {
        item.removeAttribute("contenteditable");
        if (item.textContent !== was)
          say({ id: item.id, text: item.textContent });
      },
      { once: true },
    );
  });
}

function draw() {
  const style = document.createElement("style");
  style.textContent = CSS;
  document.head.prepend(style);
  for (const item of items()) place(item);
  const layer = document.createElementNS(SVG, "svg");
  layer.setAttribute("class", "arrows");
  layer.setAttribute("aria-hidden", "true");
  document.body.prepend(layer);
  arrows(layer);
  // Fonts and pictures change the items' sizes: the arrows follow.
  const sized = new ResizeObserver(() => arrows(layer));
  for (const item of items()) sized.observe(item);
  if (parent !== window && !matchMedia("(pointer: coarse)").matches)
    edits(layer);
  return [];
}

export const ready =
  document.readyState === "loading"
    ? new Promise((done) =>
        addEventListener("DOMContentLoaded", () => done(draw())),
      )
    : Promise.resolve(draw());
