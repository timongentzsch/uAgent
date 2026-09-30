// Where streaming markdown splits into a finished head, parsed once and
// cached, and the tail still being written. Pure, so it is tested directly.
import { progressiveMarkdownScanLines } from "./limits.ts";

export function closedFence(source: string) {
  const lines = source.split("\n");
  const opening = lines[0].match(/^\s*(`{3,}|~{3,})/);
  return (
    !opening ||
    lines.slice(1).some((line) => {
      const match = line.match(/^\s*(`{3,}|~{3,})\s*$/);
      return (
        !!match &&
        match[1][0] === opening[1][0] &&
        match[1].length >= opening[1].length
      );
    })
  );
}

// Streaming split: the finished part runs through the last blank line whose
// head has balanced fences; the block still being written after it is
// rendered from `healTail`, so it reads formatted instead of as raw syntax.
// Block keys stay stable across frames (see markdownBlocks).
function balancedFences(head: string): boolean {
  const fences = head.match(/^[ \t]*(```+|~~~+).*$/gm) || [];
  let ticks = 0;
  let tildes = 0;
  for (const fence of fences) {
    if (fence.trimStart().startsWith("`")) ticks++;
    else tildes++;
  }
  return ticks % 2 === 0 && tildes % 2 === 0;
}

export function streamingHead(source: string): string {
  const lines = source.split("\n");
  const start = Math.max(1, lines.length - progressiveMarkdownScanLines);
  for (let i = lines.length - 1; i >= start; i--) {
    if (lines[i].trim() !== "") continue;
    const head = lines.slice(0, i).join("\n");
    if (!head.trim() || !balancedFences(head)) continue;
    return head;
  }
  // A code block open for longer than the scan window: split right above
  // its fence, so only the block being written re-renders each frame.
  if (balancedFences(source)) return "";
  const open = [...source.matchAll(/^[ \t]*(```+|~~~+).*$/gm)].at(-1);
  if (open?.index) {
    const head = source.slice(0, open.index - 1);
    if (head.trim() && balancedFences(head)) return head;
  }
  return "";
}
