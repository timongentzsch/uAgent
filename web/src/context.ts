import { count } from "./quantities.ts";

// Match the CLI's context summary; unknown limits never imply free capacity.
export function contextSummary(used?: number, limit?: number) {
  if (used === undefined || !Number.isFinite(used) || used < 0) return "ctx —";
  const summary = `ctx ${count(used)}`;
  if (!limit || !Number.isFinite(limit) || limit < 0) return summary;
  const remaining = Math.floor(
    (Math.max(0, Math.min(limit - used, limit)) * 100) / limit,
  );
  return `${summary}/${count(limit)} · ${remaining}% left`;
}
