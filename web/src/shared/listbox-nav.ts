// Keyboard movement through a list of n options, shared by suggestion
// listboxes and menus. From no selection (-1), down picks the first and up
// the last; both wrap. Home and End jump to the ends.
export function nextIndex(index: number, key: string, n: number) {
  if (key === "Home") return 0;
  if (key === "End") return n - 1;
  return (index + (key === "ArrowDown" ? 1 : index < 0 ? 0 : -1) + n) % n;
}

// Type-ahead: the next option after `index` (wrapping) whose label starts
// with the typed character, or -1 when none does.
export function typeAhead(labels: string[], index: number, key: string) {
  if (key.length !== 1 || !key.trim()) return -1;
  const letter = key.toLocaleLowerCase();
  const n = labels.length;
  for (let step = 1; step <= n; step++) {
    const at = (index + step + n) % n;
    if (labels[at].trim().toLocaleLowerCase().startsWith(letter)) return at;
  }
  return -1;
}

// A key without modifiers or an IME composition in progress: only these
// drive a list, so shortcuts and composed text pass through.
export const plainKey = (
  event: Pick<
    KeyboardEvent,
    "isComposing" | "shiftKey" | "ctrlKey" | "metaKey" | "altKey"
  >,
) =>
  !(
    event.isComposing ||
    event.shiftKey ||
    event.ctrlKey ||
    event.metaKey ||
    event.altKey
  );
