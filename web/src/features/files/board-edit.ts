// A person's change to a whiteboard, made in its source text: only what the
// change names is written, so the rest of the file stays as the agent wrote
// it (skills/artifact/references/whiteboard.md).

import { maxItemEntries, maxItemTextChars } from "../../shared/limits.ts";

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
interface ItemChange {
  id: string;
  at?: number[];
  size?: number[];
  text?: string;
  shape?: string;
  fill?: string;
  tag?: string;
}
// A note, a text, a section or a shape put on the board.
interface NewItem {
  id: string;
  tag: string;
  at: number[];
  size?: number[];
  note?: boolean;
  shape?: string;
  fill?: string;
  text: string;
}
interface Arrow {
  from: string;
  to: string;
}
// One change, one write: items changed together; an item added; copies of
// items under new ids; items taken away with the arrows that name them;
// items laid over the rest or under them; an arrow drawn or taken away.
export type BoardEdit =
  | { set: ItemChange[] }
  | { add: NewItem }
  | { copy: { id: string; as: string; at: number[] }[] }
  | { remove: string[] }
  | { front: string[] }
  | { back: string[] }
  | { arrow: Arrow }
  | { unarrow: Arrow };

const NEW_ID = /^[a-z][a-z0-9-]*$/;
const VOID = /^(?:img|hr|br|input)$/;
// A start tag, its quoted values read as values whatever they hold.
const TAG = /<([a-z][a-z0-9]*)\b(?:[^>"']|"[^"]*"|'[^']*')*>/gi;
const escaped = (text: string) =>
  text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/"/g, "&quot;");
const blank = (text: string) => text.replace(/[^\n]/g, " ");
// The text with what holds no tag of the page blanked, at the same offsets:
// comments, scripts, styles and the quoted values of attributes.
const masked = (source: string) =>
  source
    .replace(/<!--[\s\S]*?(?:-->|$)/g, blank)
    .replace(
      /(<(script|style)\b(?:[^>"']|"[^"]*"|'[^']*')*>)([\s\S]*?)(?=<\/\2|$)/gi,
      (_, open: string, _name, body: string) => open + blank(body),
    )
    .replace(TAG, (tag) =>
      tag.replace(/"[^"]*"|'[^']*'/g, (value) => `"${blank(value.slice(2))}"`),
    );
const pair = (numbers?: number[]) =>
  Array.isArray(numbers) &&
  numbers.length === 2 &&
  numbers.every((n) => Number.isFinite(n))
    ? numbers.map(Math.round).join(" ")
    : undefined;
// Where an attribute is in a start tag, found in its masked copy.
const attrAt = (tag: string, name: string) =>
  new RegExp(`\\s${name}(?:=("[^"]*"|'[^']*'|[^\\s>]*))?(?=[\\s/>])`, "i").exec(
    masked(tag),
  );
const attr = (tag: string, name: string) => {
  const found = attrAt(tag, name);
  if (!found) return undefined;
  const value = tag.slice(found.index, found.index + found[0].length);
  return value.replace(/^\s[^=]*=?/, "").replace(/^(["'])([\s\S]*)\1$/, "$2");
};
// The tag with the attribute set to `value`, or taken out without one.
const setAttr = (tag: string, name: string, value?: string) => {
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
interface Found {
  id: string;
  name: string;
  open: string;
  start: number;
  inner: number;
  close: number;
  end: number;
}
function elementAt(source: string, mask: string, tag: RegExpExecArray) {
  const name = tag[1].toLowerCase();
  const inner = tag.index + tag[0].length;
  const open = source.slice(tag.index, inner);
  const found = { id: attr(open, "id") ?? "", name, open, start: tag.index };
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
// The items: the elements placed by data-at, outermost; none where one has
// no end of its own (an end tag left out), which would be a guess.
function itemsOf(source: string) {
  const mask = masked(source);
  const found: Found[] = [];
  const tags = new RegExp(TAG);
  for (let tag = tags.exec(mask); tag; tag = tags.exec(mask)) {
    if (!/\sdata-at=/i.test(tag[0])) continue;
    const item = elementAt(source, mask, tag);
    if (!item || /\sdata-at=/i.test(mask.slice(item.inner, item.close)))
      return undefined;
    found.push(item);
    tags.lastIndex = item.end;
  }
  return found;
}
const arrowsOf = (source: string) =>
  [...masked(source).matchAll(/<hr\b[^>]*>/gi)].map((tag) => {
    const open = source.slice(tag.index, tag.index + tag[0].length);
    return {
      from: attr(open, "data-from"),
      to: attr(open, "data-to"),
      start: tag.index,
      end: tag.index + tag[0].length,
    };
  });

// From `start` to `end`, with their line where they stand alone on it.
function lineOf(source: string, start: number, end: number) {
  const before = source.lastIndexOf("\n", start - 1) + 1;
  const after = source.indexOf("\n", end) + 1 || source.length;
  return source.slice(before, start).trim() || source.slice(end, after).trim()
    ? [start, end]
    : [before, after];
}
// The spans cut out; one inside another goes with it.
const cut = (source: string, spans: number[][]) =>
  spans
    .filter(
      (span, index) =>
        !spans.some(
          (other, at) =>
            at !== index &&
            other[0] <= span[0] &&
            span[1] <= other[1] &&
            (other[1] - other[0] > span[1] - span[0] || at < index),
        ),
    )
    .sort((a, b) => b[0] - a[0])
    .reduce((text, [from, to]) => text.slice(0, from) + text.slice(to), source);
// `texts` put in at `at`, a line each, ended as the file ends its lines.
function put(source: string, at: number, texts: string[]) {
  const end = source.includes("\r\n") ? "\r\n" : "\n";
  return (
    source.slice(0, at) +
    (at && source[at - 1] !== "\n" ? end : "") +
    texts.map((text) => text + end).join("") +
    source.slice(at)
  );
}
// Where new items go: after the last item, or before the arrows, or the
// end of the body; and where the first item starts.
function front(source: string) {
  const last = itemsOf(source)?.at(-1);
  if (last) return lineOf(source, last.start, last.end)[1];
  const tail = masked(source).search(/<hr\b|<\/body/i);
  return tail < 0 ? source.length : source.lastIndexOf("\n", tail - 1) + 1;
}
function back(source: string) {
  const first = itemsOf(source)?.[0];
  return first ? lineOf(source, first.start, first.end)[0] : front(source);
}
const id = (value: unknown): value is string =>
  typeof value === "string" && value !== "";
// A copy's content, its own elements' ids left out: an id names one element.
const withoutIds = (text: string) => {
  const mask = masked(text);
  let made = "";
  let from = 0;
  for (const tag of mask.matchAll(TAG)) {
    const open = text.slice(tag.index, tag.index + tag[0].length);
    made += text.slice(from, tag.index) + setAttr(open, "id");
    from = tag.index + tag[0].length;
  }
  return made + text.slice(from);
};

// The item changed in place, or null where the change is not one it takes.
function change(source: string, edit: ItemChange): string | null {
  const item = itemsOf(source)?.find(
    (found) => id(edit?.id) && found.id === edit.id,
  );
  if (!item) return null;
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
  if (edit.text !== undefined) {
    if (typeof edit.text !== "string" || edit.text.length > maxItemTextChars)
      return null;
    // Text only where the item holds text alone, or one element that does
    // (a section's name).
    const mask = masked(inner);
    const one = /^(\s*<([a-z][a-z0-9]*)\b[^>]*>)[^<]*(<\/\2>\s*)$/i.exec(mask);
    const text = escaped(edit.text);
    if (one)
      inner =
        inner.slice(0, one[1].length) +
        text +
        inner.slice(inner.length - one[3].length);
    else if (!/<[a-z!/]/i.test(mask) && !VOID.test(item.name)) inner = text;
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
    add.text.length > maxItemTextChars ||
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
  // A file is a board by what it says itself, not by what its page says.
  const meta = [...masked(source).matchAll(TAG)].some(
    (tag) =>
      tag[1].toLowerCase() === "meta" &&
      attr(source.slice(tag.index, tag.index + tag[0].length), "name") ===
        "board",
  );
  if (!items || !meta) return null;
  const ids = new Set(items.map((item) => item.id));
  const few = (list: unknown): list is unknown[] =>
    Array.isArray(list) && list.length > 0 && list.length <= maxItemEntries;
  const named = (list: unknown): list is string[] =>
    few(list) && list.every((one) => id(one) && ids.has(one));
  const fresh = (value: unknown) =>
    id(value) && NEW_ID.test(value) && !ids.has(value);
  if ("set" in edit)
    return few(edit.set)
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
    const copies = few(edit.copy) ? edit.copy : [];
    const texts = copies.map((copy) => {
      const item = items.find((found) => id(copy?.id) && found.id === copy.id);
      const place = pair(copy?.at);
      if (!item || !place || !fresh(copy.as)) return undefined;
      const open = setAttr(setAttr(item.open, "id", copy.as), "data-at", place);
      return open + withoutIds(source.slice(item.inner, item.end));
    });
    const made = new Set(copies.map((copy) => copy?.as));
    if (!texts.length || made.size !== texts.length || !texts.every(Boolean))
      return null;
    // The arrows between two of the copied, between their copies.
    const as = new Map(copies.map((copy) => [copy.id, copy.as]));
    const lines = arrowsOf(source)
      .filter(({ from, to }) => as.has(from!) && as.has(to!))
      .map(({ from, to, start, end }) =>
        setAttr(
          setAttr(source.slice(start, end), "data-from", as.get(from!)),
          "data-to",
          as.get(to!),
        ),
      );
    const text = put(source, front(source), texts as string[]);
    const last = arrowsOf(text).at(-1);
    return last && lines.length
      ? put(text, lineOf(text, last.start, last.end)[1], lines)
      : text;
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
    if (rest === source || !itemsOf(rest)?.length) return null;
    return put(
      rest,
      "front" in edit ? front(rest) : back(rest),
      moving.map((item) => source.slice(item.start, item.end)),
    );
  }
  if ("arrow" in edit) {
    const { from, to } = edit.arrow ?? {};
    if (!named([from, to]) || from === to) return null;
    const last = arrowsOf(source).at(-1);
    return put(
      source,
      last ? lineOf(source, last.start, last.end)[1] : front(source),
      [`<hr data-from="${from}" data-to="${to}">`],
    );
  }
  if ("unarrow" in edit) {
    const { from, to } = edit.unarrow ?? {};
    const arrow =
      id(from) &&
      id(to) &&
      arrowsOf(source).find((one) => one.from === from && one.to === to);
    return arrow ? cut(source, [lineOf(source, arrow.start, arrow.end)]) : null;
  }
  return null;
}
