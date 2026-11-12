// A person's edit of one text block of a document or a slide, applied to its
// source. The block is found by its place among those design-helper.js lists.

const EDITABLE =
  "h1, h2, h3, h4, h5, h6, p, li, td, th, dt, dd, figcaption, blockquote";

export interface BlockEdit {
  // Its place, or NOTES for a slide's speaker notes, whose `html` is their
  // lines as text.
  index: number;
  // When the page was shown: the block's text, that of the blocks before and
  // after it (none at an end), and how many blocks there were. An edit is
  // made only where the file still has that place.
  before: string;
  around?: [string?, string?];
  of?: number;
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

// A text as two readings of it are compared: without its white space.
const squash = (text?: string | null) => String(text ?? "").replace(/\s+/g, "");

// The kind of a block that can be turned into another, from its tag and its
// parent's: an item is named by its list.
export const kindOf = (tag: string, parent: string) =>
  ["p", "h2", "h3"].includes(tag)
    ? tag
    : tag === "li" && (parent === "ul" || parent === "ol")
      ? parent
      : "";

// A slide's speaker notes, typed under its stage as lines, as an edit: of
// its last <aside>, which is no block of the slide.
const NOTES = -1;
export const notesEdit = (before: string, text: string): BlockEdit => ({
  index: NOTES,
  before,
  html: text,
});
const isSlide = (page: Document) =>
  !!page
    .querySelector('meta[name="artboard"]')
    ?.getAttribute("content")
    ?.trim()
    .endsWith(" slide");
// The lines written: each a paragraph where the aside held elements, else
// its text.
function note(aside: Element, text: string) {
  if (!aside.children.length) aside.textContent = text;
  else
    aside.replaceChildren(
      ...text
        .split("\n")
        .filter((line) => line.trim())
        .map((line) => {
          const made = aside.ownerDocument.createElement("p");
          made.textContent = line;
          return made;
        }),
    );
}

// A file's text as a page, read without the byte order mark it may start
// with. What follows </body> is parsed into the body: it is dropped there,
// so that a save adds no line.
const BOM = "\uFEFF";
function parsed(source: string) {
  const page = new DOMParser().parseFromString(
    source.startsWith(BOM) ? source.slice(1) : source,
    "text/html",
  );
  const tail = page.body.lastChild;
  if (tail?.nodeType === Node.TEXT_NODE)
    tail.textContent = tail.textContent!.trimEnd();
  return page;
}
// A page as its file's text: the document's own nodes, a line each (its
// doctype, the comments beside its html element), then the file's own
// ending, with the mark it started with and in the line ending it had. The
// line break a pre starts with, which a parser drops, is written twice.
function written(page: Document, source: string) {
  for (const kept of page.querySelectorAll("pre, textarea, listing"))
    if (kept.firstChild?.nodeValue?.startsWith("\n")) kept.prepend("\n");
  const nodes = [...page.childNodes].map((node) =>
    node instanceof Element
      ? node.outerHTML
      : new XMLSerializer()
          .serializeToString(node)
          .replace(/^<!DOCTYPE/, "<!doctype"),
  );
  const text =
    nodes.join("\n") + /\s*$/.exec(source)![0].replaceAll("\r\n", "\n");
  return (
    (source.startsWith(BOM) ? BOM : "") +
    (source.includes("\r\n") ? text.replaceAll("\n", "\r\n") : text)
  );
}

// A slide in the style of `source`: its head, its body's own attributes,
// and where its body is one element that wraps the slide, such a wrapper;
// in it a heading and a paragraph to type over.
export function blankSlide(source: string) {
  const page = parsed(source);
  page.title = "New slide";
  page.querySelector('meta[name="slide"]')?.remove();
  const [wrap, more] = [...page.body.children].filter(
    (part) => !part.matches("aside, script, style"),
  );
  const into =
    wrap && !more && !wrap.matches(EDITABLE)
      ? (wrap.cloneNode() as Element)
      : page.body;
  into.innerHTML = "<h1>Heading</h1><p>Text</p>";
  if (into !== page.body) page.body.replaceChildren(into);
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

const retag = (from: Element, tag: string) => {
  const to = from.ownerDocument.createElement(tag);
  for (const { name, value } of from.attributes) to.setAttribute(name, value);
  to.append(...from.childNodes);
  return to;
};

// The block as another kind. An item that becomes the other list's takes its
// list along; a paragraph that becomes an item joins the list before it, or
// starts one; an item that becomes a paragraph or a heading leaves its list,
// cut in two around it, a numbered one counting on after the break (one
// that counts down is left as it is).
function turn(block: Element, as: string) {
  const page = block.ownerDocument;
  const list = block.localName === "li" ? block.parentElement! : null;
  if (as === "ul" || as === "ol") {
    if (list) return list.replaceWith(retag(list, as));
    const before = block.previousElementSibling;
    const into = before?.localName === as ? before : page.createElement(as);
    if (into !== before) block.before(into);
    into.append(retag(block, "li"));
    return block.remove();
  }
  // Read as a parser reads it: what a paragraph cannot hold (a list, an
  // item's own paragraphs) follows it instead, and the empty paragraphs
  // that leaves are dropped.
  const read = page.createElement("div");
  read.innerHTML = retag(block, as).outerHTML;
  const made = [...read.childNodes].filter(
    (node) => node.nodeName !== "P" || node.hasChildNodes(),
  );
  if (!list) return block.replaceWith(...made);
  const rest = list.cloneNode() as Element;
  rest.removeAttribute("id");
  while (block.nextSibling) rest.append(block.nextSibling);
  block.remove();
  if (rest.localName === "ol" && !rest.hasAttribute("reversed")) {
    let next = (list as HTMLOListElement).start;
    for (const item of list.children) {
      const own = Number.parseInt(item.getAttribute("value") ?? "", 10);
      next = (Number.isNaN(own) ? next : own) + 1;
    }
    rest.setAttribute("start", String(next));
  }
  list.after(...made, rest);
  for (const part of [list, rest]) if (!part.children.length) part.remove();
}

// The source with the edit made, the block's text then and the place of the
// block the caret belongs in (a new one's first), or null when the file no
// longer has the edit's place: the block and those around it as the page
// showed them or as `wrote` says this last wrote them, among as many blocks
// (the file changed meanwhile).
export function applyEdit(
  source: string,
  edit: BlockEdit,
  wrote?: (index: number) => string | undefined,
) {
  const page = parsed(source);
  const slide = isSlide(page);
  const notes = slide
    ? [...page.querySelectorAll("body > aside")].pop()
    : undefined;
  // As the page lists them: what a noscript holds is text there, and a
  // slide's notes are none of its blocks.
  const blocks = () =>
    [...page.body.querySelectorAll(EDITABLE)].filter(
      (found) =>
        !found.closest("noscript") &&
        !notes?.contains(found) &&
        !found.parentElement?.closest(EDITABLE),
    );
  const all = blocks();
  const stands = (
    part: Element | undefined,
    at: number,
    shown?: string | null,
  ) => {
    const now = part && squash(part.textContent);
    return (
      now === (shown == null ? undefined : squash(shown)) ||
      (now !== undefined && now === wrote?.(at))
    );
  };
  const { index } = edit;
  const block =
    index !== NOTES
      ? all[index]
      : (notes ??
        (slide
          ? page.body.appendChild(page.createElement("aside"))
          : undefined));
  if (!block || !stands(block, index, String(edit.before))) return null;
  const [left, right] = Array.isArray(edit.around) ? edit.around : [];
  if (
    index !== NOTES &&
    ((edit.of !== undefined && edit.of !== all.length) ||
      (edit.around &&
        !(
          stands(all[index - 1], index - 1, left) &&
          stands(all[index + 1], index + 1, right)
        )))
  )
    return null;
  if (index === NOTES) note(block, String(edit.html));
  else fill(block, edit.html);
  const text = squash(block.textContent);
  // After the block, or after the whole of the list, table or figure it is
  // part of.
  let added: Element | null = null;
  if (edit.insert && Object.hasOwn(INSERTS, edit.insert)) {
    const within = (part: Element | null) =>
      part?.closest("ul, ol, dl, table, figure") ?? null;
    let whole = block;
    for (let up = within(block); up; up = within(up.parentElement)) whole = up;
    whole.insertAdjacentHTML("afterend", fresh(edit.insert));
    added = whole.nextElementSibling!;
    added = added.matches(EDITABLE) ? added : added.querySelector(EDITABLE);
  }
  const kind = kindOf(block.localName, block.parentElement!.localName);
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
// The most a closing page's write may carry: a browser sends no larger body
// for a page that is gone.
export const CLOSING_BYTES = 48 * 1024;

// What a save answers with: the place of the block the caret then belongs
// in, and, while a block typed in before is not in the file, why not.
export interface Saved {
  at: number;
  owed?: string;
}

// What is typed in a block reaches its file once typing pauses, another
// block is typed in or `flush` is called, whichever is first: one write at a
// time, each into the file as it then is. A block whose typing was refused
// stays owed, with why, until it is written or forgotten; another kind or a
// new block that was refused took nothing typed with it, and is not owed.
export function blockSaver(io: {
  read: (path: string) => Promise<{ text: string; revision?: string }>;
  write: (
    path: string,
    text: string,
    revision: string,
    closing: boolean,
  ) => Promise<{ revision: string }>;
}) {
  const key = (path: string, index: number) => `${path}\n${index}`;
  // What was last written in a block from here, and the blocks owed.
  const wrote = new Map<string, string>();
  const owed = new Map<string, string>();
  // The file as last read or written here: a closing page cannot wait for a
  // read, and writes into this.
  let seen: { path: string; text: string; revision: string } | undefined;
  let held:
    | {
        path: string;
        edit: BlockEdit;
        done: Promise<Saved>;
        start: (made: Promise<Saved>) => void;
      }
    | undefined;
  let timer: ReturnType<typeof setTimeout>;
  let last: Promise<unknown> = Promise.resolve();
  const write = async (path: string, edit: BlockEdit, closing: boolean) => {
    const mine = key(path, edit.index);
    try {
      if (!closing) {
        const { text, revision = "" } = await io.read(path);
        seen = { path, text, revision };
      }
      const next =
        seen?.path === path && seen.revision
          ? applyEdit(seen.text, edit, (index) => wrote.get(key(path, index)))
          : null;
      if (!next || (closing && new Blob([next.text]).size > CLOSING_BYTES))
        throw new Error(
          "this text changed in the file meanwhile. Reopen it to edit.",
        );
      const { revision } = await io.write(
        path,
        next.text,
        seen!.revision,
        closing,
      );
      seen = { path, text: next.text, revision };
      // Another kind or a new block moves the places after it.
      if (edit.as || edit.insert) wrote.clear();
      else wrote.set(mine, next.wrote);
      owed.delete(mine);
      return { at: next.at, owed: [...owed.values()][0] };
    } catch (error) {
      const why = error instanceof Error ? error.message : String(error);
      if (!edit.as && !edit.insert) owed.set(mine, why);
      throw new Error(`Not saved: ${why}`);
    }
  };
  // Writes what is held now; whether all that was typed is in its file.
  const flush = (closing = false) => {
    clearTimeout(timer);
    const mine = held;
    held = undefined;
    if (mine) {
      const made = last.then(() => write(mine.path, mine.edit, closing));
      mine.start(made);
      last = made.catch(() => {});
    }
    return last.then(() => !owed.size);
  };
  return {
    // Resolves once the edit, or a later one of its block, is in the file.
    save(path: string, edit: BlockEdit) {
      if (held && (held.path !== path || held.edit.index !== edit.index))
        void flush();
      if (!held) {
        let start!: (made: Promise<Saved>) => void;
        const done = new Promise<Saved>((resolve) => (start = resolve));
        held = { path, edit, done, start };
      }
      held.edit = edit;
      clearTimeout(timer);
      timer = setTimeout(flush, PAUSE_MS);
      return held.done;
    },
    flush,
    // What is owed is left: the person was told and goes, or the page was
    // drawn again from the file and no longer holds it.
    forget: () => owed.clear(),
  };
}
