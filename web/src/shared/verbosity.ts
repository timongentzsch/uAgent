import { createContext } from "preact";
import type { DetailPolicy, Verbosity } from "./types.ts";

// The host's table says what each level shows; this is only what a host too
// old to send one gets, the default level's row.
const FALLBACK: DetailPolicy = {
  work: "groups",
  reasoning: "closed",
  open: false,
  minor: false,
};

// The level in effect here and what it shows: this device's choice when it
// made one the host knows, else the global setting.
export interface Detail {
  level: string;
  policy: DetailPolicy;
}
export const defaultDetail: Detail = { level: "default", policy: FALLBACK };
export function detail(verbosity?: Verbosity, override = ""): Detail {
  const levels = verbosity?.levels || {};
  const level = levels[override] ? override : verbosity?.level || "default";
  return levels[level] ? { level, policy: levels[level] } : defaultDetail;
}

// A level's name as a control shows it.
export const levelLabel = (level: string) =>
  level.charAt(0).toUpperCase() + level.slice(1);

export const DetailContext = createContext<Detail>(defaultDetail);
