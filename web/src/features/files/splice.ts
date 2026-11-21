// A person's change written into the agent's file as a splice: only the
// changed element's text is replaced, every other byte stays as it was, so
// the agent's next text replacement still finds what it wrote.

export const VOID =
  /^(?:area|base|br|col|embed|hr|img|input|link|meta|source|track|wbr)$/;
// A start tag, its quoted values read as values whatever they hold.
export const TAG = /<([a-z][a-z0-9]*)\b(?:[^>"']|"[^"]*"|'[^']*')*>/gi;
export const escaped = (text: string) =>
  text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/"/g, "&quot;");
const blank = (text: string) => text.replace(/[^\n]/g, " ");
// The text with what holds no tag of the page blanked, at the same offsets:
// comments, scripts, styles and the quoted values of attributes.
export const masked = (source: string) =>
  source
    .replace(/<!--[\s\S]*?(?:-->|$)/g, blank)
    .replace(
      /(<(script|style)\b(?:[^>"']|"[^"]*"|'[^']*')*>)([\s\S]*?)(?=<\/\2|$)/gi,
      (_, open: string, _name, body: string) => open + blank(body),
    )
    .replace(TAG, (tag) =>
      tag.replace(/"[^"]*"|'[^']*'/g, (value) => `"${blank(value.slice(2))}"`),
    );
// Where an attribute is in a start tag, found in its masked copy.
const attrAt = (tag: string, name: string) =>
  new RegExp(`\\s${name}(?:=("[^"]*"|'[^']*'|[^\\s>]*))?(?=[\\s/>])`, "i").exec(
    masked(tag),
  );
export const attr = (tag: string, name: string) => {
  const found = attrAt(tag, name);
  if (!found) return undefined;
  const value = tag.slice(found.index, found.index + found[0].length);
  return value.replace(/^\s[^=]*=?/, "").replace(/^(["'])([\s\S]*)\1$/, "$2");
};
// The tag with the attribute set to `value`, or taken out without one.
export const setAttr = (tag: string, name: string, value?: string) => {
  const found = attrAt(tag, name);
  const written = value === undefined ? "" : ` ${name}="${escaped(value)}"`;
  if (found)
    return (
      tag.slice(0, found.index) +
      written +
      tag.slice(found.index + found[0].length)
    );
  return tag.replace(/\s*\/?>$/, (end) => `${written}${end}`);
};

// An element of the source: its start tag, where its content starts and
// its end tag, past the one that closes it among any of its kind inside.
export interface Found {
  id: string;
  name: string;
  open: string;
  start: number;
  inner: number;
  close: number;
  end: number;
}
export function elementAt(
  source: string,
  mask: string,
  tag: RegExpExecArray | RegExpMatchArray,
): Found | undefined {
  const name = tag[1].toLowerCase();
  const start = tag.index ?? 0;
  const inner = start + tag[0].length;
  const open = source.slice(start, inner);
  const found = { id: attr(open, "id") ?? "", name, open, start };
  if (VOID.test(name)) return { ...found, inner, close: inner, end: inner };
  const nested = new RegExp(`<(/?)${name}\\b[^>]*>`, "gi");
  nested.lastIndex = inner;
  let depth = 1;
  for (let at = nested.exec(mask); at; at = nested.exec(mask))
    if ((depth += at[1] ? -1 : 1) === 0)
      return {
        ...found,
        inner,
        close: at.index,
        end: at.index + at[0].length,
      };
}
