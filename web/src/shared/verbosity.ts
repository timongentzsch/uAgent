import { createContext } from "preact";
import type { DetailPolicy, Verbosity } from "./types.ts";

// The host's table says what each level shows; until the catalogue that
// carries it has loaded, the default level's row stands in.
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

export const DetailContext = createContext<Detail>(defaultDetail);
