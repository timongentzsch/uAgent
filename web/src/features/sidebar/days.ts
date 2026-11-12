// Recent's groups, split at local midnights: today, yesterday, the five days
// before yesterday, and everything earlier.
export const DAYS = ["Today", "Yesterday", "This week", "Earlier"] as const;

// The index in DAYS of a moment in milliseconds.
export function dayOf(time = 0, now = Date.now()) {
  const midnight = (back: number) => {
    const day = new Date(now);
    day.setHours(0, 0, 0, 0);
    day.setDate(day.getDate() - back);
    return day.getTime();
  };
  const index = [0, 1, 6].findIndex((back) => time >= midnight(back));
  return index < 0 ? DAYS.length - 1 : index;
}
