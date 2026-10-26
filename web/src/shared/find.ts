// Finding words in text the way the host does (src/core/strings.cc
// ContainsCaseInsensitive): only A-Z fold, so a count here and the host's
// count of the same text agree.
const fold = (text: string) => text.replace(/[A-Z]/g, (c) => c.toLowerCase());

// Where each occurrence of `query` starts in `text`, without overlaps.
export function occurrences(text: string, query: string) {
  const needle = fold(query);
  const haystack = fold(text);
  const found: number[] = [];
  if (!needle) return found;
  for (
    let at = haystack.indexOf(needle);
    at >= 0;
    at = haystack.indexOf(needle, at + needle.length)
  )
    found.push(at);
  return found;
}

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

// The CSS Highlight API, where the browser has it.
export const highlights =
  typeof CSS !== "undefined" && "highlights" in CSS ? CSS.highlights : null;
