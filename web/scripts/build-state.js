import { createHash } from "node:crypto";
import { readdir, readFile, realpath, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

// Content, rather than mtimes, survives a checkout or artifact download and
// also detects deleted inputs. Keep build and install on one definition.
export async function buildState(root) {
  const inputs = [];
  async function visit(directory) {
    for (const entry of await readdir(resolve(root, directory), {
      withFileTypes: true,
    })) {
      const path = `${directory}/${entry.name}`;
      if (entry.isDirectory()) await visit(path);
      else if (entry.isFile()) inputs.push(path);
    }
  }
  for (const directory of ["src", "public", "scripts"]) await visit(directory);
  for (const entry of await readdir(root, { withFileTypes: true })) {
    if (entry.isFile() && /\.(?:json|js|ts|html)$/.test(entry.name))
      inputs.push(entry.name);
  }
  async function digest(paths) {
    const hash = createHash("sha256");
    for (const path of paths.sort()) {
      hash.update(path).update("\0");
      hash.update(await readFile(resolve(root, path))).update("\0");
    }
    return hash.digest("hex");
  }
  return {
    source: await digest(inputs),
    dependencies: await digest(["package.json", "package-lock.json"]),
  };
}

if (
  process.argv[1] &&
  (await realpath(process.argv[1])) === fileURLToPath(import.meta.url)
) {
  const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
  const state = await buildState(root);
  const dependencyStamp = resolve(root, "node_modules/.uagent-dependencies");
  const action = process.argv[2];
  if (action === "capture") {
    await writeFile(
      resolve(root, "node_modules/.uagent-build-inputs.json"),
      JSON.stringify(state),
    );
  } else if (action === "record-dependencies") {
    await writeFile(dependencyStamp, state.dependencies);
  } else if (action === "check" || action === "check-dependencies") {
    let matches = false;
    try {
      if (action === "check") await readFile(resolve(root, "dist/.assets"));
      matches =
        action === "check"
          ? JSON.parse(await readFile(resolve(root, "dist/.build-inputs.json")))
              .source === state.source
          : (await readFile(dependencyStamp, "utf8")) === state.dependencies;
    } catch (error) {
      if (error.code !== "ENOENT" && !(error instanceof SyntaxError))
        throw error;
    }
    process.exitCode = matches ? 0 : 1;
  } else {
    throw new Error(`Unknown build-state action: ${action}`);
  }
}
