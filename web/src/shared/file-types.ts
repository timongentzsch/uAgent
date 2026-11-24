// A file a message cites, as the Files pane opens it: its lines ("12" or
// "12-20"), or its diff: among the uncommitted changes, or what a turn
// changed in it.
export interface FileTarget {
  path: string;
  sequence?: number;
  origin?: string;
  exact?: boolean;
  journal?: string;
  lines?: string;
  changes?: boolean;
  turn?: number;
}

// Small and eager, so the renderer can tell a cited path without the
// highlighter.

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
export const IMAGE = /\.(png|jpe?g|gif|webp|svg|avif)$/i;
export const VIDEO = /\.(mp4|webm|mov)$/i;
export const MARKDOWN = /\.(md|markdown)$/i;
export const HTML = /\.html?$/i;
// What a page may draw from besides its own file: styles, scripts, data,
// pictures, films and fonts.
export const SHARED =
  /\.(css|m?js|json|csv|tsv|xlsx|png|jpe?g|gif|webp|svg|avif|mp4|webm|mov|woff2?|ttf|otf)$/i;
// Excel and CSV files open as sheets, read-only.
export const SHEET_FILE = /\.(xlsx|csv)$/i;
// What opens as a page in the viewer, not as text.
export const paged = (path: string) => HTML.test(path) || SHEET_FILE.test(path);
// Files with no language that are still plainly files.
const PLAIN = /\.(txt|csv|log|lock|pdf)$/i;

export const nameOf = (path: string) => path.slice(path.lastIndexOf("/") + 1);

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
  [IMAGE, VIDEO, MARKDOWN, PLAIN].some((kind) => kind.test(nameOf(path)));

// The same path reference goes into the draft from Files or the composer.
export function fileQuote(path: string, lines = "") {
  const value = path + (lines ? `:${lines}` : "");
  const ticks = value.match(/`+/g);
  if (!ticks) return "`" + value + "`";
  const fence = "`".repeat(Math.max(...ticks.map((run) => run.length)) + 1);
  return fence + " " + value + " " + fence;
}
