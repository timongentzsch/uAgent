import { count } from "../shared/quantities.ts";

// Match the CLI's context summary; unknown limits never imply free capacity.
export function contextSummary(used?: number, limit?: number, compact = false) {
  if (used === undefined || !Number.isFinite(used) || used < 0)
    return compact ? "Context —" : "est. ctx —";
  const summary = compact
    ? `Context ${count(used)} tokens`
    : `est. ctx ${count(used)}`;
  if (!limit || !Number.isFinite(limit) || limit < 0) return summary;
  const remaining = Math.floor(
    (Math.max(0, Math.min(limit - used, limit)) * 100) / limit,
  );
  return compact
    ? `Context ${remaining}% left`
    : `${summary}/${count(limit)} · ${remaining}% left`;
}
