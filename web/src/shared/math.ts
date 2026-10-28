import type MarkdownIt from "markdown-it";
import { katex } from "@mdit/plugin-katex";
import "katex/dist/katex.min.css";
// The math plugin for the Markdown renderer, loaded on the first reply that
// has a formula.

export function install(markdown: InstanceType<typeof MarkdownIt>) {
  markdown.use(katex, {
    delimiters: "all",
    allowInlineWithSpace: false,
    mathFence: false,
    trust: false,
    throwOnError: false,
    errorColor: "currentColor",
    maxExpand: 100,
    maxSize: 20,
    output: "htmlAndMathml",
    logger: () => "ignore" as const,
  });
}
