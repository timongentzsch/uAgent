// Shared display helpers: single owner for time, text, status.
// Pure functions only — no hooks, no JSX. TS-only imports so node --test
// can load this module directly.
import { plural } from "./quantities.ts";
import type { RowState, Session, Statistics } from "./types.ts";

// The folder a session belongs to: a thread's panel folder, else its
// own working directory.
export const folderOf = (item: Session) => item.folder || item.cwd || "";

// One name per approval mode, wherever a mode is shown or chosen.
export const permissionLabels = {
  ask: "Ask",
  auto: "Auto review",
  yolo: "YOLO",
} as const;
// Any other mode asks.
export const permissionLabel = (mode?: string) =>
  permissionLabels[mode === "yolo" || mode === "auto" ? mode : "ask"];

// A word as a label starts it: "saved" reads "Saved".
export const capitalize = (text: string) =>
  text.charAt(0).toUpperCase() + text.slice(1);

export const cleanText = (text = "") =>
  text.replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, "");

// Lines added and removed in a diff, counted line by line. A tool row has
// its counts as facts; this is for a diff that comes without them.
export function diffCounts(change = ""): [number, number] {
  let added = 0;
  let removed = 0;
  change.split("\n").forEach((line, index) => {
    const role = diffLineClass(line, index === 0);
    if (role === "diff-add") added++;
    else if (role === "diff-del") removed++;
  });
  return [added, removed];
}

export const formatStat = ([added, removed]: [number, number]) =>
  added || removed ? `+${added} −${removed}` : "";

// Git-style change line roles shared by tool receipts (' '/'-'/'+'/'@'
// markers with a file header) and unified diffs ('---'/'+++'/'@@').
export function diffLineClass(line: string, first: boolean): string {
  if (first && /^(Replaced|Created|Deleted|diff --git) /.test(line))
    return "diff-head";
  if (line.startsWith("@@")) return "diff-hunk";
  if (line.startsWith("--- ") || line.startsWith("+++ ")) return "diff-head";
  const marker = line.charAt(0);
  if (marker === "+") return "diff-add";
  if (marker === "-") return "diff-del";
  if (marker === "@") return "diff-hunk";
  return "diff-ctx";
}

// A call or receipt without a finished state is still at work.
export const isRunning = (state?: RowState) => !state || state === "running";

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
