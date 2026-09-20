import type { View } from "../shared/types.ts";

// Both the main transcript and subagent threads retain the same bounded
// window. The current page wins when a server page overlaps it because it
// can contain a newer revision of a streamed message.
export function prependHistoryPage(older: View, current: View): View {
  const seen = new Set(current.blocks.map((block) => block.id));
  return {
    ...older,
    blocks: [
      ...older.blocks.filter((block) => !seen.has(block.id)),
      ...current.blocks,
    ].slice(0, 256),
  };
}
