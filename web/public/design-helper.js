// Runs first in every page the Files pane previews (the host inlines it). The
// page is sandboxed in an opaque origin; this tells the app what the page is,
// what went wrong in it and what the person picked, by postMessage, and
// applies the tweaks the app sends. Opened alone with #print, it prints.
(() => {
  const root = document.documentElement;
  const meta = (name) =>
    document.querySelector(`meta[name="${name}"]`)?.content.trim() || "";
  const artboard = () => {
    const [w, h] = meta("artboard").split("x").map(Number);
    return { w: w || 0, h: h || 0 };
  };
  const tweaks = () => {
    try {
      return JSON.parse(document.getElementById("tweaks")?.textContent || "null");
    } catch {
      return null;
    }
  };
  // A CSS variable, or a data- attribute for the page's CSS to select on.
  const tweak = (name, value) => {
    if (name.startsWith("--")) root.style.setProperty(name, String(value));
    else if (/^[a-z][a-z0-9-]{0,31}$/.test(name)) root.dataset[name] = String(value);
  };
  addEventListener("DOMContentLoaded", () => {
    for (const [name, spec] of Object.entries(tweaks() || {}))
      if (!name.startsWith("--") && spec && "default" in spec) tweak(name, spec.default);
  });

  if (parent === window) {
    if (location.hash !== "#print") return;
    // A fixed artboard prints as one page its size; a document flows.
    addEventListener("load", async () => {
      const { w, h } = artboard();
      const paper = meta("print").split(/\s+/)[1] || "letter";
      const style = document.createElement("style");
      style.textContent = `@page{size:${w && h ? `${w}px ${h}px` : paper};margin:0}html{print-color-adjust:exact;-webkit-print-color-adjust:exact}`;
      document.head.append(style);
      await document.fonts.ready;
      print();
    });
    return;
  }

  const send = (type, data) =>
    parent.postMessage({ uagentDesign: 1, type, ...data }, "*");
  const height = () => root.scrollHeight;
  const failed = (message, line) =>
    send("error", { message: String(message || "error").slice(0, 300), line: line || 0 });
  addEventListener("error", (event) => failed(event.message, event.lineno));
  addEventListener("unhandledrejection", (event) =>
    failed(event.reason?.message || event.reason, 0),
  );
  addEventListener("DOMContentLoaded", () => {
    send("ready", {
      title: document.title,
      artboard: artboard(),
      print: meta("print"),
      tweaks: tweaks(),
      height: height(),
      // Where its stylesheets are, so the app can read a design system's.
      sheets: [...document.querySelectorAll('link[rel="stylesheet"]')].map((link) => link.href),
    });
    let last = 0;
    new ResizeObserver(() => {
      if (height() !== last) send("size", { height: (last = height()) });
    }).observe(root);
  });

  // Select: the pointer marks an element, a click picks it and does nothing
  // else. The app draws both, from the element's box.
  const box = (element) => {
    const { x, y, width, height } = element.getBoundingClientRect();
    return { x, y, width, height };
  };
  const step = (element) => {
    if (element.id) return `#${CSS.escape(element.id)}`;
    const tag = element.localName;
    const same = [...element.parentElement.children].filter((e) => e.localName === tag);
    return same.length > 1 ? `${tag}:nth-of-type(${same.indexOf(element) + 1})` : tag;
  };
  const selector = (element) => {
    const steps = [];
    for (let at = element; at && at !== document.body && at !== root; at = at.parentElement) {
      steps.unshift(step(at));
      if (at.id) break;
    }
    return steps.join(" > ") || "body";
  };
  let picked = null;
  let frame = 0;
  const hover = (event) => {
    cancelAnimationFrame(frame);
    frame = requestAnimationFrame(() => send("hover", { rect: box(event.target) }));
  };
  const pick = (event) => {
    event.preventDefault();
    event.stopPropagation();
    picked = event.target;
    send("pick", {
      rect: box(picked),
      selector: selector(picked),
      tag: picked.localName,
      text: (picked.innerText || picked.getAttribute("alt") || picked.getAttribute("aria-label") || "")
        .replace(/\s+/g, " ")
        .trim()
        .slice(0, 60),
    });
  };
  const moved = () => picked && send("picked", { rect: box(picked) });
  const select = (on) => {
    const toggle = on ? addEventListener : removeEventListener;
    toggle("pointermove", hover, true);
    toggle("click", pick, true);
    toggle("scroll", moved, true);
    root.style.cursor = on ? "crosshair" : "";
    if (!on) picked = null;
  };
  addEventListener("keydown", (event) => {
    const field = event.target.closest?.("input, textarea, select, [contenteditable]");
    if (!field && ["Escape", "ArrowLeft", "ArrowRight"].includes(event.key))
      send("key", { key: event.key });
  });
  addEventListener("message", (event) => {
    if (event.source !== parent || event.data?.uagentDesign !== 1) return;
    if (event.data.type === "select") select(!!event.data.on);
    if (event.data.type === "tweak") tweak(String(event.data.name), event.data.value);
  });
})();
