import { count } from "../shared/quantities.ts";

const known = (used?: number): used is number =>
  used !== undefined && Number.isFinite(used) && used >= 0;
const share = (used: number, limit?: number) =>
  limit && Number.isFinite(limit) && limit >= 0
    ? Math.floor((Math.max(0, Math.min(limit - used, limit)) * 100) / limit)
    : undefined;

// Match the CLI's context summary; unknown limits never imply free capacity.
export function contextSummary(used?: number, limit?: number) {
  if (!known(used)) return "est. ctx —";
  const left = share(used, limit);
  return left === undefined
    ? `est. ctx ${count(used)}`
    : `est. ctx ${count(used)}/${count(limit)} · ${left}% left`;
}

// The same, short enough for the composer's one field.
export function contextLeft(used?: number, limit?: number) {
  if (!known(used)) return "Context —";
  const left = share(used, limit);
  return left === undefined ? `Context ${count(used)}` : `${left}% left`;
}
