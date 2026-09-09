import { readFile, readdir } from "node:fs/promises";
import { gzipSync } from "node:zlib";

const root = new URL("../dist/", import.meta.url);
const html = await readFile(new URL("index.html", root), "utf8");
const entry = new Set(
  [...html.matchAll(/(?:src|href)="(\/assets\/[^\"]+)"/g)].map((match) =>
    match[1].slice(1),
  ),
);
const sizes = {
  initial_js: { raw: 0, gzip: 0 },
  initial_css: { raw: 0, gzip: 0 },
  lazy_and_shell: { raw: 0, gzip: 0 },
  total: { raw: 0, gzip: 0 },
};
for (const path of await readdir(root, {
  recursive: true,
  withFileTypes: true,
})) {
  if (!path.isFile()) continue;
  const url = new URL(`${path.parentPath}/${path.name}`, "file:");
  const name = url.href.slice(root.href.length);
  const bytes = await readFile(url);
  const size = { raw: bytes.length, gzip: gzipSync(bytes).length };
  const group = entry.has(name)
    ? name.endsWith(".js")
      ? "initial_js"
      : "initial_css"
    : "lazy_and_shell";
  for (const key of ["raw", "gzip"]) {
    sizes[group][key] += size[key];
    sizes.total[key] += size[key];
  }
}
// Measured baseline is documented in docs/WEB.md; headroom is intentional.
const budgets = {
  initial_js: { raw: 56 * 1024, gzip: 21 * 1024 },
  initial_css: { raw: 16 * 1024, gzip: 4.5 * 1024 },
  total: { raw: 850 * 1024, gzip: 500 * 1024 },
};
console.log(JSON.stringify({ bytes: sizes, budgets }, null, 2));
for (const [group, limits] of Object.entries(budgets)) {
  for (const [kind, ceiling] of Object.entries(limits)) {
    if (sizes[group][kind] > ceiling)
      throw new Error(`${group} ${kind} exceeds ${ceiling} bytes`);
  }
}
