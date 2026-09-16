// Attachment display names and @-mention tokens. UI-free so node tests
// cover the naming and parsing rules directly.
export function dedupeName(
  name: string,
  taken: Set<string> | string[],
): string {
  const owned = Array.isArray(taken) ? new Set(taken) : taken;
  const clean = (name || "attachment").trim() || "attachment";
  if (!owned.has(clean)) return clean;
  const dot = clean.lastIndexOf(".");
  const stem = dot > 0 ? clean.slice(0, dot) : clean;
  const suffix = dot > 0 ? clean.slice(dot) : "";
  for (let copy = 2; ; copy += 1) {
    const candidate = `${stem} (${copy})${suffix}`;
    if (!owned.has(candidate)) return candidate;
  }
}

export interface MentionMatch {
  start: number;
  end: number;
  query: string;
}

// @-mention over attached files: the word at the caret starting with @.
// The @ must open the word (start or whitespace before it), so emails and
// paths never trigger the menu.
export function matchMention(text: string, caret: number): MentionMatch | null {
  const cursor = Math.max(0, Math.min(caret, text.length));
  let start = cursor;
  while (start > 0 && /[^\s@]/.test(text[start - 1])) start -= 1;
  if (start === 0 || text[start - 1] !== "@") return null;
  start -= 1; // include the @ itself
  if (start > 0 && !/\s/.test(text[start - 1])) return null;
  return { start, end: cursor, query: text.slice(start + 1, cursor) };
}

// Inline token stored verbatim in the draft text (survives the server echo).
// The alt text is cosmetic — render looks the asset up by id — so bracket
// characters that would break markdown image syntax are flattened here.
export function encodeMention(name: string, id: string): string {
  const alt =
    name
      .replace(/[\[\]()]/g, " ")
      .replace(/\s+/g, " ")
      .trim() || "attachment";
  return `![${alt}](attachment:${id})`;
}

export function mentionOptions<T extends { name: string }>(
  files: T[],
  query: string,
): T[] {
  const needle = query.toLowerCase();
  return files.filter((file) => file.name.toLowerCase().includes(needle));
}
