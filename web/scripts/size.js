import { readFile, readdir } from "node:fs/promises";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { gzipSync } from "node:zlib";

const root = new URL("../dist/", import.meta.url);
const html = await readFile(new URL("index.html", root), "utf8");
const entry = new Set(
  [...html.matchAll(/(?:src|href)="(\/[^\"]+\.(?:js|css))"/g)].map((match) =>
    match[1].slice(1),
  ),
);
const manifest = JSON.parse(
  await readFile(new URL(".vite/manifest.json", root), "utf8"),
);
const optional = new Set(
  JSON.parse(await readFile(new URL("renderer-assets.json", root), "utf8")).map(
    (path) => path.replace(/^\//, ""),
  ),
);
const worker = await readFile(new URL("sw.js", root), "utf8");
const precached = new Set(
  [...worker.matchAll(/"url":"([^"]+)"/g)].map((match) => match[1]),
);
const app = new Set();
function visit(key) {
  const chunk = manifest[key];
  if (!chunk || app.has(chunk.file) || key === "src/shared/diagram.tsx") return;
  app.add(chunk.file);
  for (const dependency of [
    ...(chunk.imports || []),
    ...(chunk.dynamicImports || []),
  ])
    visit(dependency);
}
visit("index.html");
const sizes = {
  diagrams: { raw: 0, gzip: 0 },
  app: { raw: 0, gzip: 0 },
  initial_js: { raw: 0, gzip: 0 },
  initial_css: { raw: 0, gzip: 0 },
  lazy_and_shell: { raw: 0, gzip: 0 },
  optional_runtime: { raw: 0, gzip: 0 },
  precache: { raw: 0, gzip: 0 },
  total: { raw: 0, gzip: 0 },
};
for (const path of await readdir(root, {
  recursive: true,
  withFileTypes: true,
})) {
  if (!path.isFile()) continue;
  const url = pathToFileURL(join(path.parentPath, path.name));
  const name = url.href.slice(root.href.length);
  if (name.startsWith(".vite/")) continue;
  const diagram =
    name.startsWith("assets/") && name.endsWith(".js") && !app.has(name);
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
    sizes[diagram ? "diagrams" : "app"][key] += size[key];
    if (optional.has(name)) sizes.optional_runtime[key] += size[key];
    if (precached.has(name) || precached.has(`/${name}`))
      sizes.precache[key] += size[key];
  }
}
// Measured baseline is documented in docs/WEB.md; headroom is intentional.
const budgets = {
  initial_js: { raw: 66 * 1024, gzip: 25 * 1024 },
  initial_css: { raw: 17 * 1024, gzip: 4.6 * 1024 },
  app: { raw: 950 * 1024, gzip: 520 * 1024 },
  diagrams: { raw: 5.2 * 1024 * 1024, gzip: 1.6 * 1024 * 1024 },
};
console.log(JSON.stringify({ bytes: sizes, budgets }, null, 2));
for (const [group, limits] of Object.entries(budgets)) {
  for (const [kind, ceiling] of Object.entries(limits)) {
    if (sizes[group][kind] > ceiling)
      throw new Error(`${group} ${kind} exceeds ${ceiling} bytes`);
  }
}
