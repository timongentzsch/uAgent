// Runs before the stylesheet and app load; an external file keeps the CSP free of inline JS.
(() => {
  let t,
    z = 100;
  // The browser can restore a stale transcript position before modules run.
  try {
    history.scrollRestoration = "manual";
  } catch {}
  try {
    t = localStorage.getItem("uagent-theme");
    const stored = JSON.parse(localStorage.getItem("uagent-zoom") || "100");
    if (typeof stored === "number" && Number.isFinite(stored))
      z = Math.min(200, Math.max(50, stored || 100));
  } catch {}
  if (t !== "light" && t !== "dark")
    t = matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  document.documentElement.dataset.theme = t;
  document.documentElement.style.setProperty("--zoom", String(z / 100));
  document.documentElement.style.setProperty(
    "--conversation-measure",
    `${(1040 * 100) / z}px`,
  );
  document.querySelector('meta[name="theme-color"]').content =
    t === "dark" ? "#000000" : "#ffffff";
})();
