// Shared display helpers: single owner for time, text, status.
// Pure functions only — no hooks, no JSX. TS-only imports so node --test
// can load this module directly.
import { duration } from "./duration.ts";
import type { Asset, Session } from "./types.ts";

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

// The uploaded files a message lists; an older message names some by a
// bare string, which has nothing to show.
export const assetsOf = (files: readonly (Asset | string)[] = []) =>
  files.filter((file): file is Asset => typeof file === "object");

// A word as a label starts it: "saved" reads "Saved".
export const capitalize = (text: string) =>
  text.charAt(0).toUpperCase() + text.slice(1);

export const cleanText = (text = "") =>
  text.replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, "");

// Lines added and removed in a change receipt: its first line is the
// summary, the rest the diff with its own +/- lines.
export function diffCounts(change = ""): [number, number] {
  let added = 0;
  let removed = 0;
  for (const line of change.split("\n").slice(1)) {
    if (line.startsWith("+") && !line.startsWith("+++")) added++;
    else if (line.startsWith("-") && !line.startsWith("---")) removed++;
  }
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

// Live vocabulary is single-sourced here: anything without a terminal
// state (started but unfinished, or facts not yet recorded) reads as
// running. Diagnostics (statistics/raw) keep the exact stored value.
// A call that went wrong, as opposed to one the user cancelled.
export const isFailedStatus = (status?: string) =>
  /fail|error|timed_out|denied/i.test(status || "");

export function isRunningStatus(status?: string): boolean {
  if (!status) return true;
  const state = status.trim().toLowerCase();
  return (
    state === "running" ||
    state === "pending" ||
    state === "not recorded" ||
    state === "starting"
  );
}

export function statusLine(parts: {
  category?: string;
  status?: string;
  duration_ms?: number | null;
}): string {
  let state = (parts.status || "").trim();
  const live = isRunningStatus(state || undefined);
  if (!state) state = parts.duration_ms != null ? "Done" : "Running\u2026";
  else if (state.toLowerCase() === "not recorded") state = "Running\u2026";
  else if (live) state = parts.duration_ms != null ? "Done" : "Running\u2026";
  const head = parts.category ? `${parts.category} \u00b7 ${state}` : state;
  if (parts.duration_ms == null) return head;
  return `${head} \u00b7 ${duration(parts.duration_ms)}`;
}
