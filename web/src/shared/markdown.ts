import MarkdownIt from "markdown-it";
import { nextFence } from "./markdown-split.ts";
import { knownFile } from "./file-types.ts";
// The Markdown renderer: markdown-it with the app's link and heading rules,
// split into keyed top-level blocks so a streaming reply re-renders only its
// tail. Loaded on demand through markdown-view.tsx.

const markdown = new MarkdownIt({ html: false, linkify: false, breaks: false });
const safeURL = (url: string) =>
  /^(https?:|mailto:)/i.test(url) || /^(?!\/\/)[/#]/.test(url);
const relative = (url: string) =>
  !!url && !/^([a-z][a-z\d+.-]*:|[/#])/i.test(url);
// A relative link is kept as a link everywhere; the render decides what it
// becomes (a file, a reference or its text alone).
markdown.validateLink = (url) => safeURL(url) || relative(url);

// A file in the files pane resolves its relative images and links in its
// own folder: `base` is that folder, `url` what serves a path in it.
// `refs` makes the paths a message cites open in the Files pane.
interface FileEnv {
  base?: string;
  url?: (path: string) => string;
  refs?: boolean;
}

// A path a message cites and the lines it names: `src/a.ts:12`,
// `a.ts:12-20` (or with an en dash), `a.ts:12:5` (the column is dropped),
// `a.ts#L12-L20`, or `src/` for a folder. It has to end in a file type the
// app knows, name a line or end in a slash, so `ledger.summary`, `1.2.3`
// and `application/json` stay code.
const REFERENCE =
  /^((?:~|\.{1,2})?\/?(?:[\w.@+-]+\/)*[\w.@+-]*)(?::(\d+)(?:[-–](\d+))?(?::\d+)?|#L(\d+)(?:-L?(\d+))?)?$/;
export function fileReference(text: string) {
  const match = REFERENCE.exec(text.trim());
  const path = match?.[1] ?? "";
  if (!match || !/\w/.test(path)) return undefined;
  const start = match[2] ?? match[4];
  const end = match[3] ?? match[5];
  if (!(knownFile(path) || start || path.endsWith("/"))) return undefined;
  return { path, lines: start && (end ? `${start}-${end}` : start) };
}
// Opens what a reference wraps: a button the surface showing it handles.
function referenceOpen({ path, lines }: { path: string; lines?: string }) {
  const escape = markdown.utils.escapeHtml;
  return `<button type="button" class="file-ref" data-file="${escape(path)}"${lines ? ` data-lines="${lines}"` : ""}>`;
}
// The reference a link to a path is, where references are made.
function referenceLink(env: unknown, href: string) {
  if (!(env as FileEnv)?.refs || (env as FileEnv).url) return undefined;
  if (!relative(href) && !/^\/(?!\/)/.test(href)) return undefined;
  try {
    return fileReference(decodeURIComponent(href));
  } catch {
    return undefined;
  }
}
// The folder-relative path `target` names from `base`, or undefined when it
// leaves the folder.
function resolvePath(base: string, target: string) {
  const parts = base ? base.split("/") : [];
  try {
    for (const part of target
      .split(/[?#]/)[0]
      .split("/")
      .map(decodeURIComponent))
      if (part === "..") {
        if (!parts.pop()) return undefined;
      } else if (part && part !== ".") parts.push(part);
  } catch {
    return undefined;
  }
  return parts.join("/");
}
// What a relative `target` in a file is: its path in the folder and the
// address that serves it; undefined in a reply or outside the folder.
function fileTarget(env: unknown, target: string) {
  const { base = "", url } = (env || {}) as FileEnv;
  const path = url && relative(target) ? resolvePath(base, target) : undefined;
  return url && path !== undefined ? { path, url: url(path) } : undefined;
}

markdown.renderer.rules.image = (tokens, index, _options, env) => {
  const token = tokens[index];
  const file = fileTarget(env, String(token.attrGet("src") ?? ""));
  const escape = markdown.utils.escapeHtml;
  return file
    ? `<img src="${escape(file.url)}" alt="${escape(token.content)}">`
    : escape(`[image: ${token.content || "remote image blocked"}]`);
};
markdown.renderer.rules.link_open = (tokens, index, options, env, renderer) => {
  const token = tokens[index];
  const href = String(token.attrGet("href") ?? "");
  const reference = referenceLink(env, href);
  if (reference) return referenceOpen(reference);
  const file = fileTarget(env, href);
  if (file) {
    // The pane opens it; a new tab gets the file itself.
    token.attrSet("href", file.url);
    token.attrSet(
      "data-file",
      href.endsWith("/") ? `${file.path}/` : file.path,
    );
  } else if (relative(href)) token.attrs = null;
  else {
    token.attrSet("rel", "noopener noreferrer");
    token.attrSet("target", "_blank");
  }
  return renderer.renderToken(tokens, index, options);
};
// The link this closes, in the inline run `tokens`.
function linkOpen(tokens: { type: string }[], index: number) {
  for (let at = index - 1; at >= 0; at--) {
    if (tokens[at].type === "link_close") return undefined;
    if (tokens[at].type === "link_open") return at;
  }
  return undefined;
}
markdown.renderer.rules.link_close = (
  tokens,
  index,
  options,
  env,
  renderer,
) => {
  const open = linkOpen(tokens, index);
  const href = open === undefined ? "" : String(tokens[open].attrGet("href"));
  return referenceLink(env, href)
    ? "</button>"
    : renderer.renderToken(tokens, index, options);
};
// A code span that is a path is a reference, but not inside a link: the
// link already is one, or leads elsewhere.
const codeInline = markdown.renderer.rules.code_inline!;
markdown.renderer.rules.code_inline = (tokens, index, options, env, self) => {
  const html = codeInline(tokens, index, options, env, self);
  const reference =
    (env as FileEnv)?.refs &&
    linkOpen(tokens, index) === undefined &&
    fileReference(tokens[index].content);
  return reference ? `${referenceOpen(reference)}${html}</button>` : html;
};
// A reply sits under the page's h1 and a section's h2: its own headings
// start at h3 (capped at h6), and keep their size by their source level.
const shiftHeading: (typeof markdown.renderer.rules)["heading_open"] = (
  tokens,
  index,
  options,
  _env,
  renderer,
) => {
  // Tokens are cached and rendered again: shift for this render only.
  const token = tokens[index];
  const source = token.tag;
  const level = Number(source.slice(1));
  if (token.nesting === 1) token.attrSet("data-level", String(level));
  token.tag = `h${Math.min(6, level + 2)}`;
  try {
    return renderer.renderToken(tokens, index, options);
  } finally {
    token.tag = source;
  }
};
markdown.renderer.rules.heading_open = shiftHeading;
markdown.renderer.rules.heading_close = shiftHeading;
// Wide tables scroll in their own wrapper instead of squeezing their columns
// to the container width (display:block on <table> would do exactly that).
markdown.renderer.rules.table_open = () => '<div class="table-scroll"><table>';
markdown.renderer.rules.table_close = () => "</table></div>";
let math: Promise<void> | undefined;
let highlighting: Promise<void> | undefined;
// The whole document in one markdown-it pass: the reference the block
// renderer below must reproduce (see markdown.test.js).
export async function renderMarkdown(text: string) {
  await Promise.all(plugins(text));
  return markdown.render(text);
}

export interface MarkdownBlock {
  key: string;
  html: string;
  source: string;
  code?: { language: string; text: string; mermaid: boolean };
}

// Math and highlighting load on first need. The async entry waits for them;
// the streaming entry renders with whatever is ready and starts the rest.
function plugins(text: string) {
  const wanted: Promise<void>[] = [];
  if (/\$|\\[([]/.test(text)) {
    math ??= import("./math.ts").then(({ install }) => install(markdown));
    wanted.push(math);
  }
  if (/```|~~~/.test(text)) {
    highlighting ??= import("./highlight.ts").then(({ install }) =>
      install(markdown),
    );
    wanted.push(highlighting);
  }
  return wanted;
}

// Parse the complete document so references and container boundaries retain
// markdown-it semantics, then render top-level token ranges independently.
// Stable source-map keys let Preact retain every unaffected DOM subtree while
// the unfinished streaming tail changes. `line` offsets the keys when `text`
// is a later part of a longer document.
export async function renderMarkdownBlocks(
  text: string,
  line = 0,
  env: Record<string, unknown> = {},
): Promise<MarkdownBlock[]> {
  await Promise.all(plugins(text));
  return markdownBlocks(text, line, env);
}

// `env` carries link references between parts parsed separately (a long
// streaming reply appends new blocks instead of re-parsing).
// `plain` leaves code unhighlighted: a fence still being written would be
// highlighted again on every frame, and is once it closes.
export function markdownBlocks(
  text: string,
  line = 0,
  env: Record<string, unknown> = {},
  plain = false,
): MarkdownBlock[] {
  void plugins(text);
  const tokens = markdown.parse(text, env);
  const lines = text.split(/\n/);
  const blocks: MarkdownBlock[] = [];
  const options = plain
    ? { ...markdown.options, highlight: null }
    : markdown.options;
  for (let start = 0; start < tokens.length; ) {
    let end = start + 1;
    if (tokens[start].nesting === 1) {
      let depth = 1;
      while (end < tokens.length && depth) {
        depth += tokens[end].nesting;
        end++;
      }
    }
    const token = tokens[start];
    const map = token.map || [0, lines.length];
    const source = lines.slice(map[0], map[1]).join("\n");
    const info =
      token.type === "fence" ? token.info.trim().split(/\s+/, 1)[0] : "";
    blocks.push({
      key: `${map[0] + line}:${token.type}`,
      source,
      html: markdown.renderer.render(tokens.slice(start, end), options, env),
      code:
        token.type === "fence" || token.type === "code_block"
          ? {
              language: info,
              text: token.content,
              mermaid: info.toLowerCase() === "mermaid",
            }
          : undefined,
    });
    start = end;
  }
  return blocks;
}

const PIPE = /^\s*\|/;
const DELIMITER = /^[\s|:]*-[\s|:-]*$/;
// Nothing but list, heading, quote or rule markers and setext underlines.
const MARKERS = /^\s*(?:(?:[-=*_+#>]+|\d+[.)]?)\s*)+$/;

// Whether the text ends inside a code fence that has not closed.
export function endsInFence(text: string): boolean {
  let open = "";
  for (const line of text.split("\n")) open = nextFence(open, line);
  return !!open;
}

// Closes what a streaming reply has left open in its last block, for display
// only, so it renders formatted instead of as raw syntax (the rules of
// Vercel's remend): emphasis, strikethrough and inline code close; a half
// link shows its text; a half image waits; `$$` closes while a single `$`
// stays literal (prices). Open code fences need nothing: markdown-it already
// runs them to the end. What is shown only ever grows, though a line can
// change what the lines above it are (a table row, a setext underline): a
// table line waits for its end, a table for its delimiter row, and a line of
// bare markers for its first word.
export function healTail(text: string): string {
  if (endsInFence(text)) return text;
  if ((text.match(/\$\$/g) || []).length % 2) return `${text}\n$$`;
  // Only the block being written is healed.
  const cut = text.search(/\n[ \t]*\n(?![\s\S]*\n[ \t]*\n)/);
  const head = cut < 0 ? "" : text.slice(0, cut + 1);
  let block = cut < 0 ? text : text.slice(cut + 1);
  const rows = block.split("\n");
  if (PIPE.test(rows.at(-1)!) || MARKERS.test(rows.at(-1)!)) rows.pop();
  const end = rows.length - (rows.at(-1) === "" ? 1 : 0);
  let table = end;
  while (table > 0 && PIPE.test(rows[table - 1])) table--;
  if (table < end && !DELIMITER.test(rows[table + 1] ?? ""))
    rows.length = table;
  block = rows.join("\n");
  // Links and images only outside code: `arr[i` inside backticks is code.
  const prose = (at: number) =>
    (block.slice(0, at).match(/`/g) || []).length % 2 === 0;
  const heal = (pattern: RegExp, replace: (...parts: string[]) => string) => {
    const match = pattern.exec(block);
    if (match && prose(match.index))
      block =
        block.slice(0, match.index) +
        replace(...match) +
        block.slice(match.index + match[0].length);
  };
  heal(/!\[[^\]]*(\]\([^)]*)?$/, () => "");
  heal(/\[([^\]]*)\]\([^)]*$/, (_, text) => text);
  heal(/(^|[^!\]\\])\[([^\]\[]+)$/, (_, before, text) => before + text);
  return head + closeInline(block);
}

// Toggles emphasis markers left to right (inline code is opaque) and closes
// what is still open, innermost first. A marker with nothing after it yet is
// dropped rather than closed.
function closeInline(block: string) {
  const stack: { marker: string; at: number }[] = [];
  const word = (character = "") => /[\p{L}\p{N}]/u.test(character);
  for (let i = 0; i < block.length; ) {
    if (block[i] === "\\") {
      i += 2;
      continue;
    }
    if (block[i] === "`") {
      const run = block.slice(i).match(/^`+/)![0];
      const close = block.indexOf(run, i + run.length);
      if (close < 0) {
        stack.push({ marker: run, at: i });
        break;
      }
      i = close + run.length;
      continue;
    }
    const marker = block.startsWith("**", i)
      ? "**"
      : block.startsWith("~~", i)
        ? "~~"
        : block[i] === "*" || block[i] === "_"
          ? block[i]
          : "";
    if (!marker) {
      i++;
      continue;
    }
    const before = block[i - 1] ?? "\n";
    const after = block[i + marker.length] ?? "";
    const lineStart = block.lastIndexOf("\n", i - 1) + 1;
    const bullet =
      marker === "*" && !block.slice(lineStart, i).trim() && after === " ";
    const inWord = marker === "_" && word(before) && word(after);
    const spaced = /\s/.test(before) && /\s/.test(after || " ");
    if (!bullet && !inWord && !spaced) {
      if (stack[stack.length - 1]?.marker === marker) stack.pop();
      else stack.push({ marker, at: i });
    }
    i += marker.length;
  }
  // A marker just typed, with nothing after it yet, waits.
  let result = block
    .replace(/\s+$/, "")
    .replace(/(^|\s)(\*\*?|~~|_|`+)$/, "$1");
  for (const { marker, at } of stack.reverse()) {
    if (result.slice(at + marker.length).trim()) result += marker;
    else result = result.slice(0, at);
  }
  return result;
}
export { safeURL };
