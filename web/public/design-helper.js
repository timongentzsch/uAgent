// Runs first in every page the Files pane previews (the host inlines it). The
// page is sandboxed in an opaque origin; this tells the app what the page is
// and what went wrong in it, by postMessage, and nothing else.
(() => {
  if (parent === window) return;
  const send = (type, data) =>
    parent.postMessage({ uagentDesign: 1, type, ...data }, "*");
  const meta = (name) =>
    document.querySelector(`meta[name="${name}"]`)?.content.trim() || "";
  const height = () => document.documentElement.scrollHeight;
  addEventListener("error", (event) =>
    send("error", {
      message: String(event.message || "error").slice(0, 300),
      line: event.lineno || 0,
    }),
  );
  addEventListener("unhandledrejection", (event) =>
    send("error", {
      message: String(event.reason?.message || event.reason).slice(0, 300),
      line: 0,
    }),
  );
  addEventListener("DOMContentLoaded", () => {
    const [w, h] = meta("artboard").split("x").map(Number);
    let tweaks = null;
    try {
      tweaks = JSON.parse(document.getElementById("tweaks")?.textContent || "null");
    } catch {}
    send("ready", {
      title: document.title,
      artboard: { w: w || 0, h: h || 0 },
      print: meta("print"),
      tweaks,
      height: height(),
    });
    let last = 0;
    new ResizeObserver(() => {
      if (height() !== last) send("size", { height: (last = height()) });
    }).observe(document.documentElement);
  });
})();
