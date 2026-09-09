import type MarkdownIt from "markdown-it";
import { katex } from "@mdit/plugin-katex";
import "katex/dist/katex.min.css";
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
