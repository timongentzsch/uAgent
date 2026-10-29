// Shared display helpers: single owner for time, text, status.
// Pure functions only — no hooks, no JSX. TS-only imports so node --test
// can load this module directly.
import { plural } from "./quantities.ts";
import type { Session, Statistics } from "./types.ts";

// The folder a session belongs to: its working directory.
export const folderOf = (item: Session) => item.cwd || "";

// One name per approval mode, wherever a mode is shown or chosen.
export const permissionLabels = {
  ask: "Ask",
  auto: "Auto review",
  yolo: "YOLO",
  plan: "Plan",
} as const;
// Any other mode asks.
export const permissionLabel = (mode?: string) =>
  permissionLabels[
    mode && mode in permissionLabels
      ? (mode as keyof typeof permissionLabels)
      : "ask"
  ];

// A word as a label starts it: "saved" reads "Saved".
export const capitalize = (text: string) =>
  text.charAt(0).toUpperCase() + text.slice(1);

export const cleanText = (text = "") =>
  text.replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, "");

export const formatStat = ([added, removed]: [number, number]) =>
  added || removed ? `+${added} −${removed}` : "";

// Git-style change line roles shared by tool changes (' '/'-'/'+'/'@'
// markers) and unified diffs ('---'/'+++'/'@@').
export function diffLineClass(line: string): string {
  if (line.startsWith("@@")) return "diff-hunk";
  if (line.startsWith("--- ") || line.startsWith("+++ ")) return "diff-head";
  const marker = line.charAt(0);
  if (marker === "+") return "diff-add";
  if (marker === "-") return "diff-del";
  if (marker === "@") return "diff-hunk";
  return "diff-ctx";
}

// A router such as OpenRouter picks the upstream per call; naming it is how
// a provider's fault is traced.
export const servedBy = (provider?: string) =>
  provider ? `served by ${provider}` : "";

// One row per upstream: the calls it served and the tool calls rejected from
// its responses.
export const servedByRows = (
  served: Statistics["served_by"] = {},
): [string, string][] =>
  Object.entries(served).map(([name, { model_calls = 0, rejected_calls }]) => [
    `Served by ${name}`,
    [
      plural(model_calls, "model call"),
      rejected_calls && plural(rejected_calls, "rejected tool call"),
    ]
      .filter(Boolean)
      .join(" · "),
  ]);
