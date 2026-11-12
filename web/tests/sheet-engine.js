// The engine as a sheet's page has it, which never calculates: a call that
// would throws. And the worker the page asks to calculate, made for each
// answer (sheet/calculate.js): nothing where the engine panics or has not
// answered by `limit`.
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
import { Worker } from "node:worker_threads";

const wasm = createRequire(import.meta.url).resolve("@ironcalc/wasm-xlsx");
export const engine = await import(wasm);
const compiled = new WebAssembly.Module(
  readFileSync(wasm.replace(/wasm\.js$/, "wasm_bg.wasm")),
);
engine.initSync({ module: compiled });
for (const name of ["evaluate", "resumeEvaluation"])
  engine.Model.prototype[name] = () => {
    throw new Error(`${name} on the page`);
  };

const WORKER = `
import { parentPort, workerData } from "node:worker_threads";
const engine = await import(${JSON.stringify(pathToFileURL(wasm).href)});
engine.initSync({ module: workerData });
const { tableOf, xlsxOf } = await import(${JSON.stringify(new URL("../sheet/book.js", import.meta.url).href)});
parentPort.once("message", ({ tabs, xlsx }) =>
  parentPort.postMessage(xlsx ? xlsxOf(engine, tabs) : tableOf(engine, tabs)),
);
`;
export const ask = (request, limit = 3000) =>
  new Promise((done) => {
    const worker = new Worker(
      new URL(`data:text/javascript,${encodeURIComponent(WORKER)}`),
      { workerData: compiled },
    );
    const end = (answer) => {
      clearTimeout(timer);
      void worker.terminate();
      done(answer);
    };
    const timer = setTimeout(end, limit);
    worker.on("message", end);
    worker.on("error", () => end());
    worker.postMessage(request);
  });
export const calculate = (tabs) => ask({ tabs });
