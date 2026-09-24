// Shared display helpers: single owner for time, text, status.
// Pure functions only — no hooks, no JSX. TS-only imports so node --test
// can load this module directly.
import { duration } from "./duration.ts";

export const cleanText = (text = "") =>
  text.replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, "");

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
  if (/^[ +-]?… /.test(line)) return "diff-cut";
  return "diff-ctx";
}

export function formatTime(iso?: string): string {
  if (!iso) return "—";
  const time = new Date(iso);
  if (Number.isNaN(time.getTime())) return "—";
  return time.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}

export function formatDateTime(iso?: string): string {
  if (!iso) return "Not recorded";
  const time = new Date(iso);
  if (Number.isNaN(time.getTime())) return "Not recorded";
  return time.toLocaleString();
}

export function stringifyArgs(args: unknown): string {
  if (typeof args === "string") return args;
  try {
    return JSON.stringify(args, null, 2);
  } catch {
    return String(args ?? "");
  }
}

// Live vocabulary is single-sourced here: anything without a terminal
// state (started but unfinished, or facts not yet recorded) reads as
// running. Diagnostics (statistics/raw) keep the exact stored value.
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
