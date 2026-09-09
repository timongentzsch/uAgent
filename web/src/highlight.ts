import type MarkdownIt from "markdown-it";
import hljs from "highlight.js/lib/core";
import javascript from "highlight.js/lib/languages/javascript";
import python from "highlight.js/lib/languages/python";
import cpp from "highlight.js/lib/languages/cpp";
import bash from "highlight.js/lib/languages/bash";
import json from "highlight.js/lib/languages/json";
import diff from "highlight.js/lib/languages/diff";
for (const [name, grammar] of Object.entries({
  javascript,
  python,
  cpp,
  bash,
  json,
  diff,
}))
  hljs.registerLanguage(name, grammar);
export function install(markdown: InstanceType<typeof MarkdownIt>) {
  markdown.options.highlight = (text, language) =>
    hljs.getLanguage(language) && text.length <= 32 * 1024
      ? hljs.highlight(text, { language, ignoreIllegals: true }).value
      : "";
}
