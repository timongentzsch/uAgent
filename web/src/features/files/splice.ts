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

// The source with what differs between `was` (parsed from it) and `now`
// (`was` changed) written in its place: an element whose attributes alone
// changed by its start tag, else the run of nodes that changed. An element
// is the start tag counted as the parser counted those of its name.
// Undefined where a place is not found; the caller reads the result back.
export function spliced(source: string, was: Document, now: Document) {
  const mask = masked(source);
  const cuts: [number, number, string][] = [];
  const spanOf = (element: Element) => {
    const name = element.localName;
    const at = [...was.getElementsByTagName(name)].indexOf(element);
    const tags = mask.matchAll(
      new RegExp(`<(${name})\\b(?:[^>"']|"[^"]*"|'[^']*')*>`, "gi"),
    );
    const tag = [...tags][at];
    return tag && elementAt(source, mask, tag);
  };
  const text = (nodes: Node[]) => {
    const made = now.createElement("template");
    made.content.append(...nodes.map((node) => node.cloneNode(true)));
    // A parser drops the line break a pre starts with.
    for (const kept of made.content.querySelectorAll("pre, textarea, listing"))
      if (kept.firstChild?.nodeValue?.startsWith("\n")) kept.prepend("\n");
    return made.innerHTML;
  };
  const walk = (from: Element, to: Element): boolean => {
    const [a, b] = [[...from.childNodes], [...to.childNodes]];
    let head = 0;
    while (head < a.length && head < b.length && a[head].isEqualNode(b[head]))
      head++;
    let tail = 0;
    while (
      tail < a.length - head &&
      tail < b.length - head &&
      a.at(-1 - tail)!.isEqualNode(b.at(-1 - tail)!)
    )
      tail++;
    const gone = a.slice(head, a.length - tail);
    const made = b.slice(head, b.length - tail);
    if (!gone.length && !made.length) return true;
    const [one, other] = [gone[0], made[0]];
    if (
      gone.length === 1 &&
      made.length === 1 &&
      one instanceof Element &&
      other instanceof Element &&
      one.localName === other.localName
    ) {
      if (one.cloneNode().isEqualNode(other.cloneNode()))
        return walk(one, other);
      const span = spanOf(one);
      const kept =
        one.childNodes.length === other.childNodes.length &&
        [...one.childNodes].every((node, at) =>
          node.isEqualNode(other.childNodes[at]),
        );
      if (span && kept) {
        const open = text([other.cloneNode()]);
        cuts.push([
          span.start,
          span.inner,
          VOID.test(one.localName)
            ? open
            : open.slice(0, -`</${one.localName}>`.length),
        ]);
        return true;
      }
    }
    const parent = spanOf(from);
    // Where a node ends or starts in the source: a text only by the element
    // beside it, or by the parent's inside where none is.
    const after = (node?: Node) =>
      node === undefined
        ? parent?.inner
        : node instanceof Element
          ? spanOf(node)?.end
          : undefined;
    const before = (node?: Node) =>
      node === undefined
        ? parent?.close
        : node instanceof Element
          ? spanOf(node)?.start
          : undefined;
    const last = gone.at(-1);
    const start =
      one instanceof Element
        ? spanOf(one)?.start
        : (after(a[head - 1]) ?? (gone.length ? undefined : before(a[head])));
    const end = !gone.length
      ? start
      : last instanceof Element
        ? spanOf(last)?.end
        : before(a[a.length - tail]);
    if (start === undefined || end === undefined) return false;
    cuts.push([start, end, text(made)]);
    return true;
  };
  if (!walk(was.documentElement, now.documentElement)) return undefined;
  const crlf = source.includes("\r\n");
  return cuts
    .sort((x, y) => y[0] - x[0])
    .reduce(
      (into, [from, to, put]) =>
        into.slice(0, from) +
        (crlf ? put.replace(/\r?\n/g, "\r\n") : put) +
        into.slice(to),
      source,
    );
}
