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
  // The panes someone resized keep their size from the first frame.
  try {
    const panes = JSON.parse(localStorage.getItem("uagent-panes") || "{}");
    for (const name of ["sidebar", "board", "files", "files-list", "terminal"])
      if (Number.isFinite(panes[name]))
        document.documentElement.style.setProperty(
          `--${name}-${name === "terminal" ? "height" : "width"}`,
          `${panes[name]}rem`,
        );
  } catch {}
  // An installed app rests on the layout viewport (trackViewport): set it
  // before the first paint, so the frame does not move when that runs.
  if (matchMedia("(display-mode: standalone)").matches) {
    const root = document.documentElement;
    root.style.setProperty("--viewport-width", `${root.clientWidth}px`);
    root.style.setProperty("--viewport-height", `${root.clientHeight}px`);
  }
  document.documentElement.dataset.theme = t;
  document.documentElement.style.setProperty("--zoom", String(z / 100));
  document.documentElement.style.setProperty(
    "--conversation-measure",
    `${(1040 * 100) / z}px`,
  );
  // Each palette's --bg, before the stylesheet that holds them has loaded.
  document.querySelector('meta[name="theme-color"]').content =
    t === "dark" ? "#000000" : "#ffffff";
  // iOS reads this once at launch; translucent draws under the status bar
  // (safe areas apply) but always with white text, so light keeps the default.
  document.querySelector(
    'meta[name="apple-mobile-web-app-status-bar-style"]',
  ).content = t === "dark" ? "black-translucent" : "default";
})();
