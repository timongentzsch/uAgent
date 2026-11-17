// A whiteboard page drawn: its items laid where their data-at puts them,
// shapes and arrows drawn, and, opened alone in the app, items moved,
// resized and retyped there, which the app writes to the file. The format:
// skills/artifact/references/whiteboard.md.

const SVG = "http://www.w3.org/2000/svg";
const FILLS = {
  yellow: "#fff1a8",
  orange: "#ffd8b0",
  pink: "#ffd3e0",
  green: "#cdeccf",
  blue: "#cfe3fb",
  grey: "#e6e6e6",
};
// Shapes CSS cannot draw, as paths in a 100 by 100 box stretched to the item.
const PATHS = {
  diamond: "M50 0L100 50L50 100L0 50Z",
  hexagon: "M15 0H85L100 50L85 100H15L0 50Z",
  parallelogram: "M15 0H100L85 100H0Z",
  database:
    "M0 12A50 12 0 0 1 100 12V88A50 12 0 0 1 0 88ZM0 12A50 12 0 0 0 100 12",
  queue:
    "M12 0H88A12 50 0 0 1 88 100H12A12 50 0 0 1 12 0ZM88 0A12 50 0 0 0 88 100",
  document: "M0 0H100V86Q75 72 50 86T0 86Z",
  cloud:
    "M27 88A20 20 0 0 1 20 50A24 24 0 0 1 52 22A22 22 0 0 1 86 40A20 20 0 0 1 80 88Z",
  person: "M50 6A14 14 0 1 1 49.9 6ZM18 96Q18 48 50 48Q82 48 82 96Z",
};
const CSS = `
html, body { margin: 0; }
body { position: relative; min-height: 100vh; overflow: hidden; background: #fafafa;
  background-image: radial-gradient(#d4d4d4 1px, transparent 1px);
  background-size: 20px 20px; font: 16px/1.35 system-ui, sans-serif; color: #1a1a1a; }
hr[data-from] { display: none; }
[data-at] { position: absolute; box-sizing: border-box; margin: 0; z-index: 2; }
section[data-at] { z-index: 0; border: 1.5px solid #9a9a9a; background: #ffffff99; padding: 12px; }
section[data-at] > :first-child { margin: 0; font-size: 18px; }
h1[data-at] { font-size: 34px; } h2[data-at] { font-size: 22px; }
small[data-at] { font-size: 13px; color: #555; display: block; }
.note { width: 160px; min-height: 160px; padding: 14px; background: ${FILLS.yellow};
  box-shadow: 0 1px 3px #0002; overflow-wrap: anywhere; }
[data-shape] { display: flex; align-items: center; justify-content: center; padding: 8px 14px;
  text-align: center; }
[data-shape] > svg.shape { position: absolute; inset: 0; width: 100%; height: 100%; z-index: -1;
  overflow: visible; }
[data-shape] > svg.shape path { fill: #fff; stroke: #1a1a1a; stroke-width: 1.5px;
  vector-effect: non-scaling-stroke; }
[data-shape="rect"], [data-shape="rounded"], [data-shape="pill"], [data-shape="ellipse"] {
  background: #fff; border: 1.5px solid #1a1a1a; }
[data-shape="rounded"] { border-radius: 14px; } [data-shape="pill"] { border-radius: 999px; }
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
svg.arrows path { fill: none; stroke: #1a1a1a; stroke-width: 1.5px; }
svg.arrows .dash { stroke-dasharray: 6 5; }
svg.arrows text { font: 13px system-ui, sans-serif; fill: #1a1a1a; paint-order: stroke;
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
  if (fill && item.matches(".note")) item.style.background = fill;
  const path = PATHS[item.dataset.shape];
  if (path && !item.querySelector(":scope > svg.shape")) {
    const svg = document.createElementNS(SVG, "svg");
    svg.setAttribute("class", "shape");
    svg.setAttribute("viewBox", "0 0 100 100");
    svg.setAttribute("preserveAspectRatio", "none");
    svg.setAttribute("aria-hidden", "true");
    const drawn = document.createElementNS(SVG, "path");
    drawn.setAttribute("d", path);
    if (fill) drawn.style.fill = fill;
    svg.append(drawn);
    item.prepend(svg);
  } else if (fill && item.dataset.shape) item.style.background = fill;
}

// The arrows, from one item's nearest side to the other's, as curves.
function arrows(layer) {
  const box = (id) => {
    const item = document.getElementById(id);
    return (
      item && {
        x: item.offsetLeft,
        y: item.offsetTop,
        w: item.offsetWidth,
        h: item.offsetHeight,
      }
    );
  };
  const end = (from, to) => {
    const [cx, cy, tx, ty] = [
      from.x + from.w / 2,
      from.y + from.h / 2,
      to.x + to.w / 2,
      to.y + to.h / 2,
    ];
    const across =
      Math.abs(tx - cx) / (from.w + to.w) >=
      Math.abs(ty - cy) / (from.h + to.h);
    return across
      ? {
          x: tx > cx ? from.x + from.w : from.x,
          y: cy,
          nx: Math.sign(tx - cx) || 1,
          ny: 0,
        }
      : {
          x: cx,
          y: ty > cy ? from.y + from.h : from.y,
          nx: 0,
          ny: Math.sign(ty - cy) || 1,
        };
  };
  layer.replaceChildren(defs());
  for (const line of document.querySelectorAll("hr[data-from][data-to]")) {
    const [a, b] = [box(line.dataset.from), box(line.dataset.to)];
    if (!a || !b) continue;
    const [p, q] = [end(a, b), end(b, a)];
    const pull = Math.max(30, Math.hypot(q.x - p.x, q.y - p.y) / 3);
    const c1 = [p.x + p.nx * pull, p.y + p.ny * pull];
    const c2 = [q.x + q.nx * pull, q.y + q.ny * pull];
    const path = document.createElementNS(SVG, "path");
    path.setAttribute("d", `M${p.x} ${p.y}C${c1} ${c2} ${q.x} ${q.y}`);
    const head = line.dataset.head ?? "end";
    if (head !== "none") path.setAttribute("marker-end", "url(#uagent-head)");
    if (head === "both") path.setAttribute("marker-start", "url(#uagent-tail)");
    if (line.hasAttribute("data-dash")) path.setAttribute("class", "dash");
    layer.append(path);
    if (line.dataset.label) {
      // The curve's middle: its point at t = 0.5.
      const text = document.createElementNS(SVG, "text");
      text.setAttribute("x", `${(p.x + 3 * c1[0] + 3 * c2[0] + q.x) / 8}`);
      text.setAttribute("y", `${(p.y + 3 * c1[1] + 3 * c2[1] + q.y) / 8}`);
      text.setAttribute("text-anchor", "middle");
      text.setAttribute("dominant-baseline", "middle");
      text.textContent = line.dataset.label;
      layer.append(text);
    }
  }
}
function defs() {
  const made = document.createElementNS(SVG, "defs");
  for (const [id, d] of [
    ["uagent-head", "M0 0L10 5L0 10Z"],
    ["uagent-tail", "M10 0L0 5L10 10Z"],
  ]) {
    const marker = document.createElementNS(SVG, "marker");
    for (const [name, value] of Object.entries({
      id,
      viewBox: "0 0 10 10",
      refX: id === "uagent-head" ? 10 : 0,
      refY: 5,
      markerWidth: 8,
      markerHeight: 8,
      orient: "auto",
    }))
      marker.setAttribute(name, `${value}`);
    const tip = document.createElementNS(SVG, "path");
    tip.setAttribute("d", d);
    tip.style.fill = "#1a1a1a";
    marker.append(tip);
    made.append(marker);
  }
  return made;
}

// Opened alone in the app: a press picks an item and a drag moves it, its
// corner resizes it, the arrows move it a pixel (with Shift ten), a double
// click retypes a note or a text, and ⌘Z undoes the last change made here.
// Each change is said to the app, which writes it.
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
      if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "z") {
        event.preventDefault();
        return say({ undo: true });
      }
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
