// A person's edit of one text block of a document or a slide, applied to its
// source. The block is found by its place among those design-helper.js lists.

const EDITABLE =
  "h1, h2, h3, h4, h5, h6, p, li, td, th, dt, dd, figcaption, blockquote";

export interface BlockEdit {
  // Its place, or NOTES for a slide's speaker notes.
  index: number;
  // The block's text when the page was shown.
  before: string;
  html: string;
  // The kind the block is to become: one of KINDS.
  as?: string;
  // What is put in after it: one of INSERTS.
  insert?: string;
}

// What a block can be turned into (an item is named by its list), and what
// each is called.
export const KINDS: Record<string, string> = {
  p: "Paragraph",
  h2: "Heading 2",
  h3: "Heading 3",
  ul: "Bulleted list",
  ol: "Numbered list",
};
// What can be put in after a block, with words to type over, and what each
// is called.
export const INSERTS: Record<string, string> = {
  ...KINDS,
  table: "Table",
  hr: "Divider",
};
const fresh = (kind: string) =>
  kind === "hr"
    ? "<hr>"
    : kind === "table"
      ? `<table><thead><tr>${"<th>Column</th>".repeat(2)}</tr></thead><tbody>${`<tr>${"<td>Cell</td>".repeat(2)}</tr>`.repeat(2)}</tbody></table>`
      : kind === "ul" || kind === "ol"
        ? `<${kind}><li>Item</li></${kind}>`
        : `<${kind}>${KINDS[kind]}</${kind}>`;
// The addresses a link made here may have: the web's, mail's, or "#" and
// the id of a place in the page.
export const LINK = /^((https?|mailto):|#\S)/i;

const words = (text?: string | null) =>
  (text ?? "").replace(/\s+/g, " ").trim();

// A slide's speaker notes, typed under its stage, as an edit: of its last
// <aside>, which is made when the slide has none.
const NOTES = -1;
export const notesEdit = (before: string, text: string): BlockEdit => ({
  index: NOTES,
  before,
  html: text.replace(/[&<>]/g, (sign) => `&#${sign.charCodeAt(0)};`),
});
const notesOf = (page: Document) =>
  [...page.querySelectorAll("body > aside")].pop() ??
  page.body.appendChild(page.createElement("aside"));

// A page as its file's text: what comes before <html> (the doctype, a
// comment) stays as `source` has it, and what follows </body>, which is
// parsed into the body, adds no line there.
function written(page: Document, source: string) {
  const tail = page.body.lastChild;
  if (tail?.nodeType === Node.TEXT_NODE)
    tail.textContent = tail.textContent!.trimEnd();
  const at = source.search(/<html[\s>]/i);
  return `${at >= 0 ? source.slice(0, at) : "<!doctype html>\n"}${page.documentElement.outerHTML}\n`;
}
const parsed = (source: string) =>
  new DOMParser().parseFromString(source, "text/html");

// A slide in the style of `source` (its head, its body's own attributes),
// with a heading and a paragraph to type over.
export function blankSlide(source: string) {
  const page = parsed(source);
  page.title = "New slide";
  page.querySelector('meta[name="slide"]')?.remove();
  page.body.innerHTML = "<h1>Heading</h1><p>Text</p>";
  return written(page, source);
}

// A slide's source with its mark to be skipped set or taken away: beside
// its artboard, where the host reads it.
export function skipSlide(source: string, skip: boolean) {
  const page = parsed(source);
  page.querySelector('meta[name="slide"]')?.remove();
  if (skip)
    page
      .querySelector('meta[name="artboard"]')
      ?.insertAdjacentHTML("afterend", '<meta name="slide" content="skip">');
  return written(page, source);
}

// The page is untrusted: an edit leaves in a block what it held, by tag and
// attributes, and of anything else only bare marks and links (to a place in
// the page, only one that is there).
function fill(block: Element, html: string) {
  const shape = (part: Element) => (part.cloneNode() as Element).outerHTML;
  const held = new Set([...block.querySelectorAll("*")].map(shape));
  block.innerHTML = html;
  for (const part of block.querySelectorAll("*")) {
    if (held.has(shape(part))) continue;
    const href = part.getAttribute("href") ?? "";
    const link =
      part.localName === "a" &&
      LINK.test(href) &&
      (href[0] !== "#" || !!block.ownerDocument.getElementById(href.slice(1)));
    if (!link && !part.matches("b, i, code, br"))
      part.replaceWith(...part.childNodes);
    else
      for (const { name } of [...part.attributes])
        if (!link || name !== "href") part.removeAttribute(name);
  }
}

// The kind of a block that can be turned into another: an item of a plain
// list is named by its list.
const kindOf = (block: Element) =>
  block.matches("p, h2, h3")
    ? block.localName
    : block.matches(":is(ul, ol) > li") && !block.querySelector("ul, ol")
      ? block.parentElement!.localName
      : "";

const retag = (from: Element, tag: string) => {
  const to = from.ownerDocument.createElement(tag);
  for (const { name, value } of from.attributes) to.setAttribute(name, value);
  to.append(...from.childNodes);
  return to;
};

// The block as another kind. An item that becomes the other list's takes its
// list along; one that becomes a paragraph or a heading leaves its list, cut
// in two around it; a paragraph that becomes an item joins the list before
// it, or starts one.
function turn(block: Element, as: string) {
  const list = block.localName === "li" ? block.parentElement! : null;
  const listed = as === "ul" || as === "ol";
  if (list && listed) return list.replaceWith(retag(list, as));
  const next = retag(block, listed ? "li" : as);
  if (list) {
    const rest = list.cloneNode() as Element;
    rest.removeAttribute("id");
    while (block.nextSibling) rest.append(block.nextSibling);
    list.after(next, rest);
    block.remove();
    for (const part of [list, rest]) if (!part.children.length) part.remove();
  } else if (listed) {
    const before = block.previousElementSibling;
    const into =
      before?.localName === as ? before : block.ownerDocument.createElement(as);
    if (into !== before) block.before(into);
    into.append(next);
    block.remove();
  } else block.replaceWith(next);
}

// The source with the edit made, the block's text then and the place of the
// block the caret belongs in (a new one's first), or null when that block
// is there neither as the page showed it nor as `wrote` (the file changed
// meanwhile).
export function applyEdit(source: string, edit: BlockEdit, wrote?: string) {
  const page = parsed(source);
  const blocks = () =>
    [...page.body.querySelectorAll(EDITABLE)].filter(
      (found) => !found.parentElement?.closest(EDITABLE),
    );
  const block = edit.index === NOTES ? notesOf(page) : blocks()[edit.index];
  const now = words(block?.textContent);
  if (!block || (now !== words(edit.before) && now !== wrote)) return null;
  fill(block, edit.html);
  const text = words(block.textContent);
  // After the block, or after the list, table or figure it is part of.
  let added: Element | null = null;
  if (edit.insert && Object.hasOwn(INSERTS, edit.insert)) {
    const whole = block.closest("ul, ol, dl, table, figure") ?? block;
    whole.insertAdjacentHTML("afterend", fresh(edit.insert));
    added = whole.nextElementSibling!;
    added = added.matches(EDITABLE) ? added : added.querySelector(EDITABLE);
  }
  const kind = kindOf(block);
  if (kind && edit.as && edit.as !== kind && Object.hasOwn(KINDS, edit.as))
    turn(block, edit.as);
  return {
    text: written(page, source),
    wrote: text,
    at: added ? blocks().indexOf(added) : edit.index,
  };
}

// How long typing pauses before it is written.
const PAUSE_MS = 1000;

// What is typed in a block reaches its file once typing pauses, another
// block is typed in or `flush` is called, whichever is first: one write at a
// time. The file is read as typing starts, so a flush is one request, which
// a page being closed can still send.
export function blockSaver(io: {
  read: (path: string) => Promise<{ text: string; revision?: string }>;
  write: (
    path: string,
    text: string,
    revision: string,
    closing: boolean,
  ) => Promise<unknown>;
}) {
  // What was last written in a block from here, by its file and place.
  const wrote = new Map<string, string>();
  let held:
    | {
        key: string;
        edit: BlockEdit;
        go: (closing: boolean) => void;
        written: Promise<number>;
      }
    | undefined;
  let timer: ReturnType<typeof setTimeout>;
  // Whether the last write was made; true again once a failed one is over.
  let last = Promise.resolve(true);
  const flush = (closing = false) => {
    clearTimeout(timer);
    held?.go(closing);
    held = undefined;
    return last;
  };
  const hold = (path: string, key: string, edit: BlockEdit) => {
    let go!: (closing: boolean) => void;
    const paused = new Promise<boolean>((resolve) => (go = resolve));
    const file = last.then(() => io.read(path));
    const mine: NonNullable<typeof held> = {
      key,
      edit,
      go,
      written: Promise.all([file, paused]).then(
        async ([{ text, revision }, closing]) => {
          const next = revision && applyEdit(text, mine.edit, wrote.get(key));
          if (!next)
            throw new Error(
              "Not saved: this text changed in the file meanwhile. Reopen it to edit.",
            );
          await io.write(path, next.text, revision!, closing);
          wrote.set(key, next.wrote);
          return next.at;
        },
      ),
    };
    const made = mine.written.then(
      () => true,
      () => false,
    );
    last = made;
    void made.then((ok) => {
      if (!ok && last === made) last = Promise.resolve(true);
    });
    return mine;
  };
  return {
    // Resolves once the edit, or a later one of its block, is in the file,
    // with the place of the block the caret then belongs in.
    save(path: string, edit: BlockEdit) {
      const key = `${path}\n${edit.index}`;
      if (held?.key !== key) {
        flush();
        held = hold(path, key, edit);
      }
      held.edit = edit;
      clearTimeout(timer);
      timer = setTimeout(flush, PAUSE_MS);
      return held.written;
    },
    // Writes what is held now; whether all that was typed is in its file.
    flush,
  };
}
