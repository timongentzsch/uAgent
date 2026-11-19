// A person's change to a whiteboard, made in its source text: only what the
// change names is written, so the rest of the file stays as the agent wrote
// it (skills/artifact/references/whiteboard.md).

// What the format offers: a shape's kinds, a note's or a shape's fills, and
// text by size.
export const SHAPES = [
  "rect",
  "rounded",
  "pill",
  "ellipse",
  "diamond",
  "hexagon",
  "parallelogram",
  "database",
  "queue",
  "document",
  "cloud",
  "person",
];
export const FILLS = ["yellow", "orange", "pink", "green", "blue", "grey"];
export const TEXTS = ["h1", "h2", "p", "small"];

// An item as it now is: placed, sized, retyped, reshaped, filled or made
// text of another size.
export interface ItemChange {
  id: string;
  at?: number[];
  size?: number[];
  text?: string;
  shape?: string;
  fill?: string;
  tag?: string;
}
// A note, a text, a section or a shape put on the board.
export interface NewItem {
  id: string;
  tag: string;
  at: number[];
  size?: number[];
  note?: boolean;
  shape?: string;
  fill?: string;
  text: string;
}
// One change, one write: items changed together; an item added; copies of
// items under new ids; items taken away with the arrows that name them;
// items laid over the rest or under them.
export type BoardEdit =
  | { set: ItemChange[] }
  | { add: NewItem }
  | { copy: { id: string; as: string; at: number[] }[] }
  | { remove: string[] }
  | { front: string[] }
  | { back: string[] };

const NEW_ID = /^[a-z][a-z0-9-]*$/;
const VOID = /^(?:img|hr|br|input)$/;
const escaped = (text: string) =>
  text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/"/g, "&quot;");
const pair = (numbers?: number[]) =>
  Array.isArray(numbers) &&
  numbers.length === 2 &&
  numbers.every((n) => Number.isFinite(n))
    ? numbers.map(Math.round).join(" ")
    : undefined;
const attr = (tag: string, name: string) => {
  const found = new RegExp(
    `\\s${name}=(?:"([^"]*)"|'([^']*)'|([^\\s>]*))`,
    "i",
  ).exec(tag);
  return found ? (found[1] ?? found[2] ?? found[3]) : undefined;
};
const setAttr = (tag: string, name: string, value: string) => {
  const was = new RegExp(`\\s${name}=("[^"]*"|'[^']*'|[^\\s>]*)`, "i");
  const written = ` ${name}="${escaped(value)}"`;
  return was.test(tag)
    ? tag.replace(was, written)
    : tag.replace(/\s*\/?>$/, (end) => `${written}${end}`);
};

// An element of the source: its start tag, where its content starts and
// its end tag, past the one that closes it among any of its kind inside.
interface Found {
  id: string;
  name: string;
  open: string;
  start: number;
  inner: number;
  close: number;
  end: number;
}
function elementAt(source: string, tag: RegExpExecArray): Found | undefined {
  const name = tag[1].toLowerCase();
  const inner = tag.index + tag[0].length;
  const found = { id: attr(tag[0], "id") ?? "", name, open: tag[0] };
  if (VOID.test(name))
    return { ...found, start: tag.index, inner, close: inner, end: inner };
  const nested = new RegExp(`<(/?)${name}\\b[^>]*>`, "gi");
  nested.lastIndex = inner;
  let depth = 1;
  for (let at = nested.exec(source); at; at = nested.exec(source))
    if ((depth += at[1] ? -1 : 1) === 0)
      return {
        ...found,
        start: tag.index,
        inner,
        close: at.index,
        end: at.index + at[0].length,
      };
}
// The items: the elements placed by data-at, outermost.
function itemsOf(source: string) {
  const found: Found[] = [];
  const tags = /<([a-z][a-z0-9]*)\b[^>]*>/gi;
  for (let tag = tags.exec(source); tag; tag = tags.exec(source)) {
    const item = /\sdata-at=/i.test(tag[0]) && elementAt(source, tag);
    if (!item) continue;
    found.push(item);
    tags.lastIndex = item.end;
  }
  return found;
}
const arrowsOf = (source: string) =>
  [...source.matchAll(/<hr\b[^>]*>/gi)].map((tag) => ({
    from: attr(tag[0], "data-from"),
    to: attr(tag[0], "data-to"),
    start: tag.index,
    end: tag.index + tag[0].length,
  }));

// From `start` to `end`, with their line where they stand alone on it.
function lineOf(source: string, start: number, end: number) {
  const before = source.lastIndexOf("\n", start - 1) + 1;
  const after = source.indexOf("\n", end) + 1 || source.length;
  return source.slice(before, start).trim() || source.slice(end, after).trim()
    ? [start, end]
    : [before, after];
}
const cut = (source: string, spans: number[][]) =>
  spans
    .sort((a, b) => b[0] - a[0])
    .reduce((text, [from, to]) => text.slice(0, from) + text.slice(to), source);
// `texts` put in at `at`, a line each.
const put = (source: string, at: number, texts: string[]) =>
  source.slice(0, at) +
  (at && source[at - 1] !== "\n" ? "\n" : "") +
  texts.map((text) => `${text}\n`).join("") +
  source.slice(at);
// Where new items go: after the last item, or before the arrows, or the
// end of the body; and where the first item starts.
function front(source: string) {
  const last = itemsOf(source).at(-1);
  if (last) return lineOf(source, last.start, last.end)[1];
  const tail = source.search(/<hr\b|<\/body/i);
  return tail < 0 ? source.length : source.lastIndexOf("\n", tail - 1) + 1;
}
function back(source: string) {
  const first = itemsOf(source)[0];
  return first ? lineOf(source, first.start, first.end)[0] : front(source);
}

// The item changed in place, or null where the change is not one it takes.
function change(source: string, edit: ItemChange): string | null {
  const item = itemsOf(source).find((found) => found.id === edit?.id);
  if (!item?.id) return null;
  let open = item.open;
  let inner = source.slice(item.inner, item.close);
  let close = source.slice(item.close, item.end);
  const at = pair(edit.at);
  const size = pair(edit.size);
  if ((edit.at && !at) || (edit.size && !size)) return null;
  if (at) open = setAttr(open, "data-at", at);
  if (size) open = setAttr(open, "data-size", size);
  const note = /\bnote\b/.test(attr(open, "class") ?? "");
  const shaped = attr(open, "data-shape") !== undefined;
  if (edit.shape !== undefined) {
    if (!shaped || !SHAPES.includes(edit.shape)) return null;
    open = setAttr(open, "data-shape", edit.shape);
  }
  if (edit.fill !== undefined) {
    if (!(shaped || note) || !FILLS.includes(edit.fill)) return null;
    open = setAttr(open, "data-fill", edit.fill);
  }
  if (edit.tag !== undefined) {
    if (note || !TEXTS.includes(item.name) || !TEXTS.includes(edit.tag))
      return null;
    open = open.replace(/^<[a-z0-9]+/i, `<${edit.tag}`);
    close = `</${edit.tag}>`;
  }
  if (typeof edit.text === "string") {
    // Text only where the item holds text alone, or a heading that does.
    const heading = /^(\s*<([a-z][a-z0-9]*)\b[^>]*>)[^<]*(<\/\2>\s*)$/i.exec(
      inner,
    );
    const text = escaped(edit.text);
    if (heading) inner = heading[1] + text + heading[3];
    else if (!inner.includes("<") && !VOID.test(item.name)) inner = text;
    else return null;
  }
  return (
    source.slice(0, item.start) + open + inner + close + source.slice(item.end)
  );
}

// A new item as the format writes it, or undefined where it is not one.
function written(add: NewItem) {
  const at = pair(add?.at);
  const size = add?.size === undefined ? "" : pair(add.size);
  const kind = add?.note
    ? add.tag === "p" && !add.shape
    : add?.shape
      ? add.tag === "div" && SHAPES.includes(add.shape)
      : [...TEXTS, "section"].includes(add?.tag);
  if (
    !kind ||
    !at ||
    size === undefined ||
    typeof add.text !== "string" ||
    (add.fill !== undefined && !(add.note || add.shape)) ||
    (add.fill !== undefined && !FILLS.includes(add.fill))
  )
    return;
  const attrs = [
    ["id", add.id],
    add.note && ["class", "note"],
    add.shape && ["data-shape", add.shape],
    add.fill && ["data-fill", add.fill],
    ["data-at", at],
    size && ["data-size", size],
  ].filter((pair): pair is string[] => !!pair);
  const text = escaped(add.text);
  return `<${add.tag}${attrs.map(([name, value]) => ` ${name}="${value}"`).join("")}>${add.tag === "section" ? `<h2>${text}</h2>` : text}</${add.tag}>`;
}

// The source with the edit made, or null where the file has no such item
// (the agent changed it meanwhile) or the edit is not one a board takes.
export function boardEdit(source: string, edit: BoardEdit): string | null {
  const items = itemsOf(source);
  const ids = new Set(items.map((item) => item.id));
  const named = (list: unknown): list is string[] =>
    Array.isArray(list) && list.length > 0 && list.every((id) => ids.has(id));
  const fresh = (id: string) => NEW_ID.test(String(id)) && !ids.has(id);
  if ("set" in edit)
    return Array.isArray(edit.set) && edit.set.length
      ? edit.set.reduce<string | null>(
          (text, one) => text && change(text, one),
          source,
        )
      : null;
  if ("add" in edit) {
    const text = fresh(edit.add?.id) && written(edit.add);
    return text ? put(source, front(source), [text]) : null;
  }
  if ("copy" in edit) {
    const copies = Array.isArray(edit.copy) ? edit.copy : [];
    const texts = copies.map(({ id, as, at }) => {
      const item = items.find((found) => found.id === id);
      const place = pair(at);
      if (!item || !place || !fresh(as)) return undefined;
      const open = setAttr(setAttr(item.open, "id", as), "data-at", place);
      return open + source.slice(item.inner, item.end);
    });
    const made = new Set(copies.map(({ as }) => as));
    return texts.length && made.size === texts.length && texts.every(Boolean)
      ? put(source, front(source), texts as string[])
      : null;
  }
  if ("remove" in edit) {
    if (!named(edit.remove)) return null;
    const gone = new Set(edit.remove);
    return cut(source, [
      ...items
        .filter((item) => gone.has(item.id))
        .map((item) => lineOf(source, item.start, item.end)),
      ...arrowsOf(source)
        .filter(({ from, to }) => gone.has(from!) || gone.has(to!))
        .map((arrow) => lineOf(source, arrow.start, arrow.end)),
    ]);
  }
  if ("front" in edit || "back" in edit) {
    const list = "front" in edit ? edit.front : edit.back;
    if (!named(list)) return null;
    const moving = items.filter((item) => list.includes(item.id));
    const rest = cut(
      source,
      moving.map((item) => lineOf(source, item.start, item.end)),
    );
    if (rest === source || !itemsOf(rest).length) return null;
    return put(
      rest,
      "front" in edit ? front(rest) : back(rest),
      moving.map((item) => source.slice(item.start, item.end)),
    );
  }
  return null;
}
