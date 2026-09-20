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

// Parse the complete document so references and container boundaries retain
// markdown-it semantics, then render top-level token ranges independently.
// Stable source-map keys let Preact retain every unaffected DOM subtree while
// the unfinished streaming tail changes.
export async function renderMarkdownBlocks(
  text: string,
): Promise<MarkdownBlock[]> {
  if (/\$|\\[([]/.test(text)) {
    math ??= import("./math.ts").then(({ install }) => install(markdown));
    await math;
  }
  if (/```|~~~/.test(text)) {
    highlighting ??= import("./highlight.ts").then(({ install }) =>
      install(markdown),
    );
    await highlighting;
  }
  const env: Record<string, unknown> = {};
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
      key: `${map[0]}:${token.type}`,
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
export { safeURL };
