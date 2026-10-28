import type MarkdownIt from "markdown-it";
import hljs from "highlight.js/lib/core";
import javascript from "highlight.js/lib/languages/javascript";
import typescript from "highlight.js/lib/languages/typescript";
import python from "highlight.js/lib/languages/python";
import cpp from "highlight.js/lib/languages/cpp";
import bash from "highlight.js/lib/languages/bash";
import json from "highlight.js/lib/languages/json";
import diff from "highlight.js/lib/languages/diff";
import xml from "highlight.js/lib/languages/xml";
import css from "highlight.js/lib/languages/css";
import yaml from "highlight.js/lib/languages/yaml";
import ini from "highlight.js/lib/languages/ini";
import markdown from "highlight.js/lib/languages/markdown";
import go from "highlight.js/lib/languages/go";
import rust from "highlight.js/lib/languages/rust";
import dockerfile from "highlight.js/lib/languages/dockerfile";
import cmake from "highlight.js/lib/languages/cmake";
import {
  maxFileHighlightChars,
  maxHighlightCacheEntries,
  maxHighlightChars,
} from "./limits.ts";
for (const [name, grammar] of Object.entries({
  javascript,
  typescript,
  python,
  cpp,
  bash,
  json,
  diff,
  xml,
  css,
  yaml,
  ini,
  markdown,
  go,
  rust,
  dockerfile,
  cmake,
}))
  hljs.registerLanguage(name, grammar);
// Least recently used first: a Map iterates in insertion order.
const highlighted = new Map<string, string>();
function highlightBlock(text: string, language: string) {
  const key = `${language}\0${text}`;
  const kept = highlighted.get(key);
  if (kept !== undefined) {
    highlighted.delete(key);
    highlighted.set(key, kept);
    return kept;
  }
  const html = hljs.highlight(text, { language, ignoreIllegals: true }).value;
  highlighted.set(key, html);
  const oldest = highlighted.keys().next();
  if (highlighted.size > maxHighlightCacheEntries && !oldest.done)
    highlighted.delete(oldest.value);
  return html;
}
export function install(markdown: InstanceType<typeof MarkdownIt>) {
  markdown.options.highlight = (text, language) =>
    hljs.getLanguage(language) && text.length <= maxHighlightChars
      ? highlightBlock(text, language)
      : "";
}

// A file's language, by its name or else its extension.
const NAMED: Record<string, string> = {
  "CMakeLists.txt": "cmake",
  Dockerfile: "dockerfile",
};
const EXTENSIONS: Record<string, string> = {
  bash: "bash",
  c: "cpp",
  cc: "cpp",
  cjs: "javascript",
  cmake: "cmake",
  cpp: "cpp",
  css: "css",
  diff: "diff",
  go: "go",
  h: "cpp",
  hpp: "cpp",
  html: "xml",
  ini: "ini",
  js: "javascript",
  json: "json",
  jsonl: "json",
  jsx: "javascript",
  md: "markdown",
  mjs: "javascript",
  patch: "diff",
  py: "python",
  rs: "rust",
  sh: "bash",
  svg: "xml",
  toml: "ini",
  ts: "typescript",
  tsx: "typescript",
  xml: "xml",
  yaml: "yaml",
  yml: "yaml",
  zsh: "bash",
};
export function languageFor(path: string) {
  const name = path.slice(path.lastIndexOf("/") + 1);
  const dot = name.lastIndexOf(".");
  return (
    NAMED[name] ||
    (dot > 0 && EXTENSIONS[name.slice(dot + 1).toLowerCase()]) ||
    ""
  );
}

const escape = (text: string) =>
  text.replace(
    /[&<>]/g,
    (character) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" })[character]!,
  );

// A file as lines of HTML, highlighted by its name. A span a line break
// cuts through (a block comment, a template string) is closed at the end
// of the line and opened again on the next, so each line stands alone.
export function highlightLines(text: string, path: string) {
  const language = languageFor(path);
  const html =
    language && text.length <= maxFileHighlightChars
      ? hljs.highlight(text, { language, ignoreIllegals: true }).value
      : escape(text);
  const lines: string[] = [];
  const open: string[] = [];
  let line = "";
  for (const [, start, end, run] of html.matchAll(
    /(<span[^>]*>)|(<\/span>)|([^<]+)/g,
  )) {
    if (start) open.push(start);
    if (end) open.pop();
    const [first, ...rest] = (run ?? start ?? end).split("\n");
    line += first;
    for (const next of rest) {
      lines.push(line + "</span>".repeat(open.length));
      line = open.join("") + next;
    }
  }
  // A final newline ends the last line rather than starting another.
  if (line !== open.join("") || !lines.length)
    lines.push(line + "</span>".repeat(open.length));
  return lines;
}
