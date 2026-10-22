import { applyMotion, applyTheme, applyZoom, normalizeZoom } from "./layout.ts";
import { defaultZoom } from "./limits.ts";
import { stored, type Stored } from "./storage.ts";
import { storageKeys } from "./storage-keys.ts";
import { defaultTimePrefs, normalizeTimePrefs } from "./time.ts";

// The docked panes someone can resize (shared/side-pane.tsx); each width
// is kept in rem, so it follows the text size.
export type Pane = "sidebar" | "board" | "files" | "files-list";

// This browser's preferences.
export const devicePrefs = {
  theme: stored(storageKeys.theme, "system"),
  motion: stored(storageKeys.motion, "system"),
  zoom: stored(storageKeys.zoom, defaultZoom, (value) =>
    normalizeZoom(typeof value === "number" ? value : defaultZoom),
  ),
  time: stored(storageKeys.time, defaultTimePrefs, normalizeTimePrefs),
  panes: stored<Partial<Record<Pane, number>>>(storageKeys.panes, {}),
};

// Applies the display preferences to the document now and whenever one
// changes, wherever it is changed.
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
}
