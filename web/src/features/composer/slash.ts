import type { SlashCommand } from "../../shared/types.ts";

export function slashMatches(commands: SlashCommand[], text: string) {
  return /^\/[^\s]*$/.test(text)
    ? commands.filter(
        (entry) => !entry.terminal && entry.command.startsWith(text),
      )
    : [];
}

export function slashCompletion(matches: SlashCommand[]) {
  let prefix = matches[0]?.command || "";
  for (const entry of matches)
    while (!entry.command.startsWith(prefix)) prefix = prefix.slice(0, -1);
  return prefix + (matches.length === 1 && matches[0].argument ? " " : "");
}

export function parseSlash(commands: SlashCommand[], text: string) {
  const [name, argument = ""] = text.trim().split(/\s+(.*)/s);
  const spec = commands.find(
    (entry) => entry.command === name || entry.aliases.includes(name),
  );
  return { name: spec?.command || name, argument };
}

// How well `query` matches `text`, lower being better, or -1 when it does
// not: its letters in order (a subsequence), ranked prefix first, then a
// word's start, then by how tightly the letters sit.
export function fuzzyScore(query: string, text: string) {
  const needle = query.trim().toLowerCase();
  const haystack = text.toLowerCase();
  if (haystack.startsWith(needle)) return 0;
  if ([" ", "/", ".", "_", "-"].some((gap) => haystack.includes(gap + needle)))
    return 1;
  let at = -1,
    first = -1;
  for (const letter of needle) {
    at = haystack.indexOf(letter, at + 1);
    if (at < 0) return -1;
    if (first < 0) first = at;
  }
  return 2 + (at - first + 1 - needle.length) / haystack.length;
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
