// What a file is by its name: the language it is highlighted in, whether it
// is an image or Markdown, and whether a name is a file at all. Small and
// eager, so the renderer can tell a cited path without the highlighter.

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
export const IMAGE = /\.(png|jpe?g|gif|webp|svg)$/i;
export const MARKDOWN = /\.(md|markdown)$/i;
export const HTML = /\.html?$/i;
// Files with no language that are still plainly files.
const PLAIN = /\.(txt|csv|log|lock|pdf)$/i;

// A path's last part: the file or folder it names.
export const nameOf = (path: string) => path.slice(path.lastIndexOf("/") + 1);

// A file's language, by its name or else its extension.
export function languageFor(path: string) {
  const name = nameOf(path);
  const dot = name.lastIndexOf(".");
  return (
    NAMED[name] ||
    (dot > 0 && EXTENSIONS[name.slice(dot + 1).toLowerCase()]) ||
    ""
  );
}

// Whether a name is a file of a kind the app knows: what `ledger.summary`
// or `1.2.3` is not.
export const knownFile = (path: string) =>
  !!languageFor(path) ||
  [IMAGE, MARKDOWN, PLAIN].some((kind) => kind.test(nameOf(path)));
