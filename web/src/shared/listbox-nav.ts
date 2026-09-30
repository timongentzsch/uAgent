// Keyboard movement through a list of n options, shared by suggestion
// listboxes and menus. From no selection (-1), down picks the first and up
// the last; both wrap. Home and End jump to the ends.
export function nextIndex(index: number, key: string, n: number) {
  if (key === "Home") return 0;
  if (key === "End") return n - 1;
  return (index + (key === "ArrowDown" ? 1 : index < 0 ? 0 : -1) + n) % n;
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
