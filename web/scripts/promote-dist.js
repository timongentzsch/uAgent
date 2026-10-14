import { readdir, readFile, rename, rm, writeFile } from "node:fs/promises";
import { extname, relative, resolve } from "node:path";
import { gzipSync } from "node:zlib";

const root = resolve(import.meta.dirname, "..");
const current = resolve(root, "dist");
const next = resolve(root, "dist.next");
const previous = resolve(root, "dist.previous");

async function files(directory) {
  const result = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    if (entry.name === ".vite" || entry.name === ".assets") continue;
    const path = resolve(directory, entry.name);
    if (entry.isDirectory()) result.push(...(await files(path)));
    else if (entry.isFile()) result.push(relative(next, path));
  }
  return result;
}

const assets = (await files(next)).sort();
await writeFile(resolve(next, ".assets"), `${assets.join("\n")}\n`);

// The binary embeds the `.gz` beside each text asset and serves it as is;
// images and fonts are already compressed and stay raw.
const text = new Set([".html", ".js", ".css", ".json", ".webmanifest", ".svg"]);
for (const asset of assets) {
  if (!text.has(extname(asset))) continue;
  const path = resolve(next, asset);
  await writeFile(`${path}.gz`, gzipSync(await readFile(path), { level: 9 }));
}

await rm(previous, { recursive: true, force: true });
try {
  await rename(current, previous);
} catch (error) {
  if (error.code !== "ENOENT") throw error;
}
try {
  await rename(next, current);
} catch (error) {
  try {
    await rename(previous, current);
  } catch {}
  throw error;
}
await rm(previous, { recursive: true, force: true });
