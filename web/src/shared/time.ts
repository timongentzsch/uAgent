// One owner for how moments read. Every timestamp renders through <Time>,
// which formats with these preferences; nothing else calls toLocale*.
import { createContext } from "preact";
import { useEffect, useState } from "preact/hooks";

export interface TimePrefs {
  // System follows the locale; 12 and 24 force the clock either way.
  clock: "system" | "12" | "24";
  // Smart: the clock today, the day as it gets older. Relative: "5 min ago".
  // Absolute: the full date and time on every row.
  style: "smart" | "relative" | "absolute";
}
export const defaultTimePrefs: TimePrefs = { clock: "system", style: "smart" };
export const TimePrefsContext = createContext<TimePrefs>(defaultTimePrefs);

export function normalizeTimePrefs(value: unknown): TimePrefs {
  const prefs = (value || {}) as Partial<TimePrefs>;
  return {
    clock: ["system", "12", "24"].includes(prefs.clock as string)
      ? (prefs.clock as TimePrefs["clock"])
      : defaultTimePrefs.clock,
    style: ["smart", "relative", "absolute"].includes(prefs.style as string)
      ? (prefs.style as TimePrefs["style"])
      : defaultTimePrefs.style,
  };
}

const formatters = new Map<string, Intl.DateTimeFormat>();
function format(
  value: Date,
  prefs: TimePrefs,
  options: Intl.DateTimeFormatOptions,
) {
  const hourCycle =
    prefs.clock === "12" ? "h12" : prefs.clock === "24" ? "h23" : undefined;
  const key = JSON.stringify([options, hourCycle]);
  let formatter = formatters.get(key);
  if (!formatter) {
    formatter = new Intl.DateTimeFormat(undefined, { ...options, hourCycle });
    formatters.set(key, formatter);
  }
  return formatter.format(value);
}

const relative = new Intl.RelativeTimeFormat(undefined, { numeric: "auto" });
const units: [Intl.RelativeTimeFormatUnit, number][] = [
  ["second", 60],
  ["minute", 60],
  ["hour", 24],
  ["day", 30],
  ["month", 12],
  ["year", Infinity],
];

export function formatRelative(value: Date, now: Date): string {
  let amount = (value.getTime() - now.getTime()) / 1000;
  for (const [unit, size] of units) {
    if (Math.abs(amount) < size || unit === "year") {
      // Under a minute reads as "now" rather than a ticking second count.
      return unit === "second"
        ? relative.format(0, "second")
        : relative.format(Math.round(amount), unit);
    }
    amount /= size;
  }
  return "";
}

const day = (value: Date) =>
  new Date(value.getFullYear(), value.getMonth(), value.getDate()).getTime();
const capitalize = (text: string) =>
  text.charAt(0).toLocaleUpperCase() + text.slice(1);

export function formatMoment(
  value: Date,
  prefs: TimePrefs,
  now = new Date(),
): string {
  const clock = { hour: "numeric", minute: "2-digit" } as const;
  if (prefs.style === "relative") return formatRelative(value, now);
  const days = Math.round((day(now) - day(value)) / 86_400_000);
  if (prefs.style === "absolute" || days < 0)
    return format(value, prefs, { dateStyle: "medium", timeStyle: "short" });
  if (days === 0) return format(value, prefs, clock);
  if (days === 1)
    return `${capitalize(relative.format(-1, "day"))} ${format(value, prefs, clock)}`;
  if (days < 7) return format(value, prefs, { weekday: "short", ...clock });
  if (value.getFullYear() === now.getFullYear())
    return format(value, prefs, { month: "short", day: "numeric", ...clock });
  return format(value, prefs, { dateStyle: "medium" });
}

// The tooltip: everything, including seconds and the time zone.
export function formatFullMoment(value: Date, prefs: TimePrefs): string {
  return format(value, prefs, { dateStyle: "full", timeStyle: "long" });
}

// One shared minute tick for every stamp whose text can change on its own.
const listeners = new Set<() => void>();
let ticker: ReturnType<typeof setInterval> | undefined;
export function useNow(live: boolean): Date {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    if (!live) return;
    const tick = () => setNow(new Date());
    listeners.add(tick);
    ticker ??= setInterval(() => listeners.forEach((each) => each()), 60_000);
    return () => {
      listeners.delete(tick);
      if (!listeners.size && ticker) {
        clearInterval(ticker);
        ticker = undefined;
      }
    };
  }, [live]);
  return now;
}
