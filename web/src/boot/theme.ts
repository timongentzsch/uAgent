import { defaultZoom } from "../shared/limits.ts";
import { zoomSteps } from "../shared/layout.ts";
import { panes, storageKeys } from "../shared/storage-keys.ts";

interface Kept {
  keys: { theme: string; zoom: string; panes: string };
  zoom: { least: number; most: number; fallback: number };
  panes: readonly string[];
  // Each palette's --bg, before the stylesheet that holds them has loaded.
  bars: { light: string; dark: string };
}

// Runs before the stylesheet and the app load, as /theme.js: an external
// file keeps the CSP free of inline JS. It is written out with what it
// needs (themeScript), since no module has loaded yet.
function boot({ keys, zoom, panes, bars }: Kept) {
  let t: string | null = null;
  let z = zoom.fallback;
  // The browser can restore a stale transcript position before modules run.
  try {
    history.scrollRestoration = "manual";
  } catch {}
  try {
    t = localStorage.getItem(keys.theme);
    const stored = JSON.parse(
      localStorage.getItem(keys.zoom) || String(zoom.fallback),
    );
    if (typeof stored === "number" && Number.isFinite(stored))
      z = Math.min(zoom.most, Math.max(zoom.least, stored || zoom.fallback));
  } catch {}
  const chosen = t === "light" || t === "dark";
  if (!chosen)
    t = matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  // The panes someone resized keep their size from the first frame.
  try {
    const sizes = JSON.parse(localStorage.getItem(keys.panes) || "{}");
    for (const name of panes)
      if (Number.isFinite(sizes[name]))
        document.documentElement.style.setProperty(
          `--${name}-${name === "terminal" ? "height" : "width"}`,
          `${sizes[name]}rem`,
        );
  } catch {}
  // An installed app rests on the layout viewport (trackViewport): set it
  // before the first paint, so the frame does not move when that runs.
  if (matchMedia("(display-mode: standalone)").matches) {
    const root = document.documentElement;
    root.style.setProperty("--viewport-width", `${root.clientWidth}px`);
    root.style.setProperty("--viewport-height", `${root.clientHeight}px`);
  }
  document.documentElement.dataset.theme = t as string;
  document.documentElement.style.setProperty("--zoom", String(z / 100));
  // dataset.bg keeps each scheme's own for the system theme (applyTheme).
  for (const meta of document.querySelectorAll<HTMLMetaElement>(
    'meta[name="theme-color"]',
  )) {
    const scheme = meta.media.includes("dark") ? "dark" : "light";
    meta.dataset.bg = bars[scheme];
    meta.content = chosen ? bars[t as "light" | "dark"] : bars[scheme];
  }
  // iOS reads this once at launch; translucent draws under the status bar
  // (safe areas apply) but always with white text, so light keeps the default.
  document.querySelector<HTMLMetaElement>(
    'meta[name="apple-mobile-web-app-status-bar-style"]',
  )!.content = t === "dark" ? "black-translucent" : "default";
}

// A palette's --bg in tokens.css: the dark one is the root's, the light one
// is set where the root is light.
const background = (tokens: string, palette: RegExp) => {
  const found = tokens.match(palette)?.[1];
  if (!found) throw new Error(`tokens.css has no --bg for ${palette}`);
  return found;
};

// The script /theme.js serves: boot, called with the keys, zoom range, panes
// and backgrounds the app itself keeps; `tokens` is tokens.css.
export const themeScript = (tokens: string) =>
  `(${boot})(${JSON.stringify({
    keys: {
      theme: storageKeys.theme,
      zoom: storageKeys.zoom,
      panes: storageKeys.panes,
    },
    zoom: {
      least: zoomSteps[0],
      most: zoomSteps[zoomSteps.length - 1],
      fallback: defaultZoom,
    },
    panes,
    bars: {
      dark: background(tokens, /\n {2}:root,[^{]*\{[^}]*?--bg: (#\w+);/),
      light: background(
        tokens,
        /\n {2}:root\[data-theme="light"\],[^{]*\{[^}]*?--bg: (#\w+);/,
      ),
    },
  } satisfies Kept)});\n`;
