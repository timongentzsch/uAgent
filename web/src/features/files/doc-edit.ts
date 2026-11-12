// A person's edit of one text block of a document or a slide, applied to its
// source. The block is found by its place among those design-helper.js lists.

const EDITABLE =
  "h1, h2, h3, h4, h5, h6, p, li, td, th, dt, dd, figcaption, blockquote";

export interface BlockEdit {
  index: number;
  // The block's text when the page was shown.
  before: string;
  html: string;
}

const words = (text?: string | null) =>
  (text ?? "").replace(/\s+/g, " ").trim();

// The source with the edit made and the block's text then, or null when that
// block is there neither as the page showed it nor as `wrote` (the file
// changed meanwhile).
export function applyEdit(source: string, edit: BlockEdit, wrote?: string) {
  const page = new DOMParser().parseFromString(source, "text/html");
  const block = [...page.body.querySelectorAll(EDITABLE)].filter(
    (found) => !found.parentElement?.closest(EDITABLE),
  )[edit.index];
  const now = words(block?.textContent);
  if (!block || (now !== words(edit.before) && now !== wrote)) return null;
  block.innerHTML = edit.html;
  // What follows </body> is parsed into the body: a save adds no line there.
  const tail = page.body.lastChild;
  if (tail?.nodeType === Node.TEXT_NODE)
    tail.textContent = tail.textContent!.trimEnd();
  // What comes before <html> (the doctype, a comment) stays as written.
  const at = source.search(/<html[\s>]/i);
  return {
    text: `${at >= 0 ? source.slice(0, at) : "<!doctype html>\n"}${page.documentElement.outerHTML}\n`,
    wrote: words(block.textContent),
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
        written: Promise<void>;
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
    const mine = {
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
    // Resolves once the edit, or a later one of its block, is in the file.
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
