// How well `query` matches `text`, lower being better, or -1 when it does
// not: its letters in order (a subsequence), ranked prefix first, then a
// word's start, then by how tightly the letters sit. Letters spread wider
// than twice the query do not match, so a long text does not match every
// word.
export function fuzzyScore(query: string, text: string) {
  const needle = query.trim().toLowerCase();
  const haystack = text.toLowerCase();
  if (haystack.startsWith(needle)) return 0;
  if ([" ", "/", ".", "_", "-"].some((gap) => haystack.includes(gap + needle)))
    return 1;
  let span = Infinity;
  for (
    let first = haystack.indexOf(needle[0]);
    first >= 0;
    first = haystack.indexOf(needle[0], first + 1)
  ) {
    let at = first;
    for (const letter of needle.slice(1)) {
      at = haystack.indexOf(letter, at + 1);
      if (at < 0) break;
    }
    if (at < 0) break;
    span = Math.min(span, at - first + 1);
  }
  if (span > 2 * needle.length) return -1;
  return 2 + (span - needle.length) / haystack.length;
}

// The items that match, best first; equal matches keep their order.
export function fuzzy<T>(items: T[], query: string, text: (item: T) => string) {
  return items
    .map((item, index) => ({
      item,
      index,
      score: fuzzyScore(query, text(item)),
    }))
    .filter((entry) => entry.score >= 0)
    .sort((a, b) => a.score - b.score || a.index - b.index)
    .map((entry) => entry.item);
}

// Whether every word of `query` occurs in one of `fields`, ignoring case; an
// empty query matches everything.
export function matches(query: string, ...fields: (string | undefined)[]) {
  const text = fields.join(" ").toLocaleLowerCase();
  return query
    .toLocaleLowerCase()
    .trim()
    .split(/\s+/)
    .every((word) => text.includes(word));
}
