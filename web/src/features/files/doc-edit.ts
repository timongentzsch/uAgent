// A person's edit of one text block of a document, applied to its source. The
// block is found by its place among those design-helper.js lists.

const EDITABLE =
  "h1, h2, h3, h4, h5, h6, p, li, td, th, dt, dd, figcaption, blockquote";

export interface BlockEdit {
  index: number;
  before: string;
  html: string;
}

const words = (text: string | null) => (text ?? "").replace(/\s+/g, " ").trim();

// The source with the edit made, or null when that block is not there as
// it was (the file changed since it was shown).
export function applyEdit(source: string, edit: BlockEdit) {
  const page = new DOMParser().parseFromString(source, "text/html");
  const block = [...page.body.querySelectorAll(EDITABLE)].filter(
    (found) => !found.parentElement?.closest(EDITABLE),
  )[edit.index];
  if (!block || words(block.textContent) !== words(edit.before)) return null;
  block.innerHTML = edit.html;
  // What comes before <html> (the doctype, a comment) stays as written.
  const at = source.search(/<html[\s>]/i);
  return `${at >= 0 ? source.slice(0, at) : "<!doctype html>\n"}${page.documentElement.outerHTML}\n`;
}
