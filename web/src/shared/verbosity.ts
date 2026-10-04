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

// The level in effect and what it shows.
export interface Detail {
  level: string;
  policy: DetailPolicy;
}
export const defaultDetail: Detail = { level: "default", policy: FALLBACK };
export function detail(verbosity?: Verbosity): Detail {
  const level = verbosity?.level || "default";
  const policy = verbosity?.levels[level];
  return policy ? { level, policy } : defaultDetail;
}

// A level's name as a control shows it.
export const levelLabel = (level: string) =>
  level.charAt(0).toUpperCase() + level.slice(1);

export const DetailContext = createContext<Detail>(defaultDetail);
