// Where streaming markdown splits into a finished head, parsed once and
// cached, and the tail still being written. Pure, so it is tested directly.

const FENCE = /^[ \t]*(`{3,}|~{3,})/;
const CLOSING = /^[ \t]*(`{3,}|~{3,})[ \t]*$/;

// The fence run still open after `line`, given the one open before it ("" for
// none). Only a bare run of at least the opening's length and character
// closes a fence. No markdown-it, so composer code may use it.
export function nextFence(open: string, line: string): string {
  if (!open) return line.match(FENCE)?.[1] ?? "";
  const close = line.match(CLOSING)?.[1];
  return close?.[0] === open[0] && close.length >= open.length ? "" : open;
}

export function closedFence(source: string) {
  const lines = source.split("\n");
  let open = lines[0].match(FENCE)?.[1] ?? "";
  return (
    !open || lines.slice(1).some((line) => !(open = nextFence(open, line)))
  );
}

// Streaming split: the finished part runs through the last complete blank
// line outside a code fence, or up to a code fence still open; the block
// still being written after it is rendered from `healTail`, so it reads
// formatted instead of as raw syntax. Block keys stay stable across frames
// (see markdownBlocks). One forward pass from `start`, the start of a line
// outside any fence; past 0 what precedes it is a head, so it has content.
function scanHead(source: string, start: number): string {
  let head = 0;
  let content = start > 0;
  let fence = "";
  let opened = 0;
  const read = (line: string) => {
    const inside = fence;
    fence = nextFence(fence, line);
    if (inside) return;
    // A code block still open at the end splits right above its fence, so
    // only the block being written re-renders each frame.
    if (fence) opened = content ? start - 1 : 0;
    content ||= !!line.trim();
  };
  for (let end = source.indexOf("\n", start); end >= 0; ) {
    const line = source.slice(start, end);
    if (!fence && !line.trim()) {
      if (content) head = start - 1;
    } else read(line);
    start = end + 1;
    end = source.indexOf("\n", start);
  }
  // The last line is still being written: it may yet open or close a
  // fence, but it is not known to be blank until it ends.
  read(source.slice(start));
  return source.slice(0, fence && opened > head + 1 ? opened : head);
}

// `from`, an earlier head of the same reply, is where the scan resumes, so a
// frame reads only what arrived since; past it a head only grows, and with
// none found there the whole reply is read.
export function streamingHead(source: string, from = ""): string {
  if (from && source.startsWith(from)) {
    const head = scanHead(source, from.length + 1);
    if (head.length === from.length) return from;
    if (head) return head;
  }
  return scanHead(source, 0);
}
