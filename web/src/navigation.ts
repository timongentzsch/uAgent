export const selectedFromURL = () =>
  new URLSearchParams(location.hash.slice(1)).get("session") || "";

// Same-document navigation preserves the shell, connection and browser history.
export function writeSelection(id: string, replace = false) {
  const hash = id ? `#session=${encodeURIComponent(id)}` : "";
  if (location.hash === hash) return;
  const url = `${location.pathname}${location.search}${hash}`;
  history[replace ? "replaceState" : "pushState"](null, "", url);
}
