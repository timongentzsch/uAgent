import { occurrences } from "../../sheet/text.js";

// The occurrences of `query` in the text under `root`, as ranges for the CSS
// Highlight API. A match split across two text nodes (say, half of it bold)
// is not found, as a find bar in a browser does not find it either.
export function rangesIn(root: Node, query: string) {
  const ranges: Range[] = [];
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    for (const at of occurrences(node.textContent || "", query)) {
      const range = document.createRange();
      range.setStart(node, at);
      range.setEnd(node, at + query.length);
      ranges.push(range);
    }
  }
  return ranges;
}

export const highlights =
  typeof CSS !== "undefined" && "highlights" in CSS ? CSS.highlights : null;
