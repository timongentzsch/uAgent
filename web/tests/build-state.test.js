import { test } from "node:test";
import assert from "node:assert/strict";
import {
  appendFile,
  chmod,
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { delimiter, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { buildState } from "../scripts/build-state.js";

test("install rebuilds changed frontend inputs and reuses unchanged dependencies", async () => {
  const root = await mkdtemp(resolve(tmpdir(), "uagent install "));
  const web = resolve(root, "web");
  const log = resolve(root, "commands.jsonl");
  try {
    for (const path of [
      "bin",
      "build/release",
      "web/src",
      "web/public",
      "web/scripts",
    ])
      await mkdir(resolve(root, path), { recursive: true });
    await copyFile(
      new URL("../../install.sh", import.meta.url),
      resolve(root, "install.sh"),
    );
    await copyFile(
      new URL("../scripts/build-state.js", import.meta.url),
      resolve(web, "scripts/build-state.js"),
    );
    // A checkout, which the installer builds instead of a release.
    await writeFile(resolve(root, "CMakeLists.txt"), "");
    await writeFile(
      resolve(root, "build/release/CMakeCache.txt"),
      "configured",
    );
    await writeFile(resolve(web, "package.json"), '{"type":"module"}');
    await writeFile(resolve(web, "package-lock.json"), "{}");
    await writeFile(resolve(web, "src/app.tsx"), "first source");
    await writeFile(resolve(web, "public/icon.svg"), "first icon");
    await writeFile(resolve(web, "vite.config.js"), "first config");
    const command = `#!/usr/bin/env node
const fs = require("node:fs");
const path = require("node:path");
const { pathToFileURL } = require("node:url");
(async () => {
  const tool = path.basename(process.argv[1]);
  const args = process.argv.slice(2);
  fs.appendFileSync(process.env.INSTALL_TEST_LOG, JSON.stringify([tool, ...args]) + "\\n");
  if (tool !== "npm") return;
  const root = args[args.indexOf("--prefix") + 1];
  if (args[0] === "ci") fs.mkdirSync(path.join(root, "node_modules"), {recursive:true});
  else {
    if (process.env.INSTALL_TEST_FAIL_BUILD) process.exit(1);
    const { buildState } = await import(pathToFileURL(path.join(root, "scripts/build-state.js")));
    fs.mkdirSync(path.join(root, "dist"), {recursive:true});
    fs.writeFileSync(path.join(root, "dist/.assets"), "index.html\\n");
    fs.writeFileSync(path.join(root, "dist/.build-inputs.json"), JSON.stringify(await buildState(root)));
  }
})().catch(error => {console.error(error); process.exitCode = 1;});
`;
    for (const tool of ["npm", "cmake"]) {
      const path = resolve(root, "bin", tool);
      await writeFile(path, command);
      await chmod(path, 0o755);
    }
    async function install(extra = {}) {
      await writeFile(log, "");
      const result = spawnSync("/bin/sh", [resolve(root, "install.sh")], {
        encoding: "utf8",
        env: {
          ...process.env,
          PATH: `${resolve(root, "bin")}${delimiter}${process.env.PATH}`,
          UAGENT_BUILD_DIR: resolve(root, "build/release"),
          UAGENT_PREFIX: resolve(root, "prefix"),
          INSTALL_TEST_LOG: log,
          UAGENT_NONINTERACTIVE: "1",
          ...extra,
        },
      });
      const calls = (await readFile(log, "utf8"))
        .trim()
        .split("\n")
        .filter(Boolean)
        .map(JSON.parse);
      return {
        result,
        calls,
        npm: calls
          .filter(([tool]) => tool === "npm")
          .map(([, action]) => action),
      };
    }
    let run = await install();
    assert.equal(run.result.status, 0, run.result.stderr);
    assert.deepEqual(run.npm, ["ci", "run"]);
    assert.deepEqual((await install()).npm, []);

    // Existing output must not hide edits, deleted inputs, or build config.
    for (const input of [
      "src/app.tsx",
      "public/icon.svg",
      "vite.config.js",
      "scripts/build-state.js",
    ]) {
      await appendFile(resolve(web, input), "\n// changed\n");
      run = await install();
      assert.equal(run.result.status, 0, run.result.stderr);
      assert.deepEqual(run.npm, ["run"], input);
    }
    await rm(resolve(web, "public/icon.svg"));
    assert.deepEqual((await install()).npm, ["run"]);
    await rm(resolve(web, "dist/.assets"));
    assert.deepEqual((await install()).npm, ["run"]);

    await writeFile(resolve(web, "package-lock.json"), '{"lockfileVersion":3}');
    assert.deepEqual((await install()).npm, ["ci", "run"]);
    await appendFile(resolve(web, "src/app.tsx"), "changed again");
    run = await install({ INSTALL_TEST_FAIL_BUILD: "1" });
    assert.notEqual(run.result.status, 0);
    assert.equal(
      run.calls.some(([tool]) => tool === "cmake"),
      false,
    );
    assert.deepEqual((await install()).npm, ["run"]);
    const state = await buildState(web);
    assert.deepEqual(
      JSON.parse(await readFile(resolve(web, "dist/.build-inputs.json"))),
      state,
    );

    // Editing while Vite runs must not label an old output as up to date.
    await copyFile(
      new URL("../scripts/promote-dist.js", import.meta.url),
      resolve(web, "scripts/promote-dist.js"),
    );
    await mkdir(resolve(web, "dist.next"));
    await writeFile(resolve(web, "dist/index.html"), "old bundle");
    await writeFile(resolve(web, "dist.next/index.html"), "new bundle");
    const capture = () =>
      spawnSync(
        process.execPath,
        [resolve(web, "scripts/build-state.js"), "capture"],
        { encoding: "utf8" },
      );
    const promote = () =>
      spawnSync(process.execPath, [resolve(web, "scripts/promote-dist.js")], {
        encoding: "utf8",
      });
    assert.equal(capture().status, 0);
    await appendFile(resolve(web, "src/app.tsx"), "edited during build");
    const refused = promote();
    assert.notEqual(refused.status, 0);
    assert.match(refused.stderr, /inputs changed during the build/);
    assert.equal(
      await readFile(resolve(web, "dist/index.html"), "utf8"),
      "old bundle",
    );
    assert.equal(capture().status, 0);
    const promoted = promote();
    assert.equal(promoted.status, 0, promoted.stderr);
    assert.equal(
      await readFile(resolve(web, "dist/index.html"), "utf8"),
      "new bundle",
    );
    assert.deepEqual(
      JSON.parse(await readFile(resolve(web, "dist/.build-inputs.json"))),
      await buildState(web),
    );
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
