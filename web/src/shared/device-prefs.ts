import { applyMotion, applyTheme, applyZoom, normalizeZoom } from "./layout.ts";
import { defaultZoom } from "./limits.ts";
import { stored, type Stored } from "./storage.ts";
import { storageKeys } from "./storage-keys.ts";
import { defaultTimePrefs, normalizeTimePrefs } from "./time.ts";
import { detailLevels } from "./tree.ts";

// The docked panes someone can resize (shared/side-pane.tsx); each width
// (the terminal's height) is kept in rem, so it follows the text size.
export type Pane = "sidebar" | "files" | "files-list" | "terminal";

export const devicePrefs = {
  theme: stored(storageKeys.theme, "system"),
  motion: stored(storageKeys.motion, "system"),
  zoom: stored(storageKeys.zoom, defaultZoom, (value) =>
    normalizeZoom(typeof value === "number" ? value : defaultZoom),
  ),
  time: stored(storageKeys.time, defaultTimePrefs, normalizeTimePrefs),
  panes: stored<Partial<Record<Pane, number>>>(storageKeys.panes, {}),
  // Empty until this device chooses, so it starts at the host's level.
  detail: stored<string>(
    storageKeys.detail,
    "",
    (value) => detailLevels.find((level) => level === value) ?? "",
  ),
  // Empty until this device chooses, so it starts as the host has it.
  thinking: stored<"" | "show" | "hide">(storageKeys.thinking, "", (value) =>
    value === "show" || value === "hide" ? value : "",
  ),
  // By conversation and node id; choosing a level forgets them.
  toggles: stored<Record<string, boolean>>(storageKeys.toggles, {}),
  list: stored<"recent" | "places">(storageKeys.list, "recent", (value) =>
    value === "places" ? value : "recent",
  ),
};

export function applyDevicePrefs() {
  const follow = <T>(store: Stored<T>, apply: (value: T) => unknown) => {
    let undo = apply(store.read());
    store.subscribe((value) => {
      if (typeof undo === "function") undo();
      undo = apply(value);
    });
  };
  follow(devicePrefs.theme, applyTheme);
  follow(devicePrefs.motion, applyMotion);
  follow(devicePrefs.zoom, applyZoom);
  // Fields measure their font when they mount, which can be before the
  // page's styles have loaded; once they have, they measure again.
  if (document.readyState !== "complete") {
    addEventListener("load", () => applyZoom(devicePrefs.zoom.read()), {
      once: true,
    });
  }
}
