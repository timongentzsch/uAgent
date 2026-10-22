import { normalizeZoom } from "./layout.ts";
import { defaultZoom } from "./limits.ts";
import { stored } from "./storage.ts";
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
