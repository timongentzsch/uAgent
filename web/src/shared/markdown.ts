import MarkdownIt from "markdown-it";
const markdown = new MarkdownIt({ html: false, linkify: false, breaks: false });
const safeURL = (url: string) =>
  /^(https?:|mailto:)/i.test(url) || /^(?!\/\/)[/#]/.test(url);
markdown.validateLink = safeURL;
markdown.renderer.rules.image = (tokens, index) =>
  markdown.utils.escapeHtml(
    `[image: ${tokens[index].content || "remote image blocked"}]`,
  );
markdown.renderer.rules.link_open = (
  tokens,
  index,
  options,
  _env,
  renderer,
) => {
  tokens[index].attrSet("rel", "noopener noreferrer");
  tokens[index].attrSet("target", "_blank");
  return renderer.renderToken(tokens, index, options);
};
// Wide tables scroll in their own wrapper instead of squeezing their columns
// to the container width (display:block on <table> would do exactly that).
markdown.renderer.rules.table_open = () => '<div class="table-scroll"><table>';
markdown.renderer.rules.table_close = () => "</table></div>";
let math: Promise<void> | undefined;
let highlighting: Promise<void> | undefined;
export async function renderMarkdown(text: string) {
  return (await renderMarkdownBlocks(text)).map((block) => block.html).join("");
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
): Promise<MarkdownBlock[]> {
  await Promise.all(plugins(text));
  return markdownBlocks(text, line);
}

// `env` carries link references between parts parsed separately (a long
// streaming reply appends new blocks instead of re-parsing).
export function markdownBlocks(
  text: string,
  line = 0,
  env: Record<string, unknown> = {},
): MarkdownBlock[] {
  void plugins(text);
  const tokens = markdown.parse(text, env);
  const lines = text.split(/\n/);
  const blocks: MarkdownBlock[] = [];
  for (let start = 0; start < tokens.length;) {
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
      html: markdown.renderer.render(
        tokens.slice(start, end),
        markdown.options,
        env,
      ),
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

const FENCE = /^[ \t]*(`{3,}|~{3,})/;

// Closes what a streaming reply has left open in its last block, for display
// only, so it renders formatted instead of as raw syntax (the rules of
// Vercel's remend): emphasis, strikethrough and inline code close; a half
// link shows its text; a half image and a half table row wait; `$$` closes
// while a single `$` stays literal (prices). Open code fences need nothing:
// markdown-it already runs them to the end.
export function healTail(text: string): string {
  const lines = text.split("\n");
  let open = "";
  for (const line of lines) {
    const fence = line.match(FENCE)?.[1];
    if (!fence) continue;
    if (!open) open = fence;
    else if (fence[0] === open[0] && fence.length >= open.length) open = "";
  }
  if (open) return text;
  if ((text.match(/\$\$/g) || []).length % 2) return `${text}\n$$`;
  // Only the block being written is healed.
  const cut = text.search(/\n[ \t]*\n(?![\s\S]*\n[ \t]*\n)/);
  const head = cut < 0 ? "" : text.slice(0, cut + 1);
  let block = cut < 0 ? text : text.slice(cut + 1);
  const rows = block.split("\n");
  const last = rows[rows.length - 1];
  if (/^\s*\|/.test(last) && rows.length > 1 && !/\|\s*$/.test(last)) {
    rows.pop();
    block = rows.join("\n");
  }
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
  for (let i = 0; i < block.length;) {
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
