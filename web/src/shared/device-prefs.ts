import { normalizeZoom } from "./layout.ts";
import { defaultZoom } from "./limits.ts";
import { storage } from "./storage.ts";
import {
  defaultTimePrefs,
  normalizeTimePrefs,
  type TimePrefs,
} from "./time.ts";

// This browser's preferences: where each is kept, what it is when nothing
// is kept, and reading and writing it. The pre-paint script
// (public/theme.js) cannot import this and repeats three of the keys.
function kept<T>(
  key: string,
  fallback: T,
  read: (text: string | null) => T,
  text: (value: T) => string = JSON.stringify,
) {
  return {
    fallback,
    read: () => {
      try {
        return read(storage.getItem(key));
      } catch {
        return fallback;
      }
    },
    write: (value: T) => {
      try {
        storage.setItem(key, text(value));
      } catch {
        /* Private storage may be full. */
      }
    },
  };
}
const parsed = (text: string | null) => JSON.parse(text || "null");

// The docked panes someone can resize (shared/side-pane.tsx); each width
// is kept in rem, so it follows the text size.
export type Pane = "sidebar" | "board" | "files" | "files-list";

export const devicePrefs = {
  theme: kept("uagent-theme", "system", (text) => text || "system", String),
  motion: kept("uagent-motion", "system", (text) => text || "system", String),
  zoom: kept("uagent-zoom", defaultZoom, (text) => {
    const value = parsed(text);
    return normalizeZoom(typeof value === "number" ? value : defaultZoom);
  }),
  time: kept<TimePrefs>("uagent-time", defaultTimePrefs, (text) =>
    normalizeTimePrefs(parsed(text)),
  ),
  panes: kept<Partial<Record<Pane, number>>>("uagent-panes", {}, (text) => {
    const value = parsed(text);
    return value && typeof value === "object" ? value : {};
  }),
};
