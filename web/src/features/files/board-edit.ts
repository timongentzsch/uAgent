// A person's change to a whiteboard's item, made in its source text: only
// the item's own attributes or text change, so the rest of the file stays
// as the agent wrote it (skills/artifact/references/whiteboard.md).

export interface BoardEdit {
  id: string;
  at?: number[];
  size?: number[];
  text?: string;
}

const escaped = (text: string) =>
  text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/"/g, "&quot;");
const pair = (numbers?: number[]) =>
  Array.isArray(numbers) &&
  numbers.length === 2 &&
  numbers.every((n) => Number.isFinite(n))
    ? numbers.map(Math.round).join(" ")
    : undefined;

// The source with the edit made, or null where the file has no such item
// (the agent changed it meanwhile) or the edit is not one a board takes.
export function boardEdit(source: string, edit: BoardEdit): string | null {
  if (!/^[\w-]+$/.test(String(edit.id))) return null;
  const tag = new RegExp(
    `<([a-z][a-z0-9]*)\\b[^>]*?\\sid=(["']?)${edit.id}\\2(?=[\\s/>])[^>]*>`,
    "i",
  ).exec(source);
  if (!tag || !/\sdata-at=/i.test(tag[0])) return null;
  let open = tag[0];
  const set = (name: string, value: string) => {
    const was = new RegExp(`\\s${name}=("[^"]*"|'[^']*'|[^\\s>]*)`, "i");
    const written = ` ${name}="${escaped(value)}"`;
    open = was.test(open)
      ? open.replace(was, written)
      : open.replace(/\s*\/?>$/, (end) => `${written}${end}`);
  };
  const at = pair(edit.at);
  const size = pair(edit.size);
  if (edit.at && !at) return null;
  if (edit.size && !size) return null;
  if (at) set("data-at", at);
  if (size) set("data-size", size);
  const start = tag.index;
  const after = start + tag[0].length;
  if (typeof edit.text !== "string")
    return source.slice(0, start) + open + source.slice(after);
  // Text only where the item holds text alone, up to its end tag.
  const close = source.indexOf(`</${tag[1]}`, after);
  if (close < 0 || source.slice(after, close).includes("<")) return null;
  return (
    source.slice(0, start) + open + escaped(edit.text) + source.slice(close)
  );
}
