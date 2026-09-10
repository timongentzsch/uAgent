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
let math: Promise<void> | undefined;
let highlighting: Promise<void> | undefined;
export async function renderMarkdown(text: string) {
  // Only the block that needs a renderer pays for its module and fonts.
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
  return markdown.render(text);
}
export { safeURL };
