// What a formula being typed is offered (sheet/suggest.js), with the engine
// itself: its list of functions is the engine's, read as the engine reads.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { FUNCTIONS } from "../sheet/functions.js";
import { suggestions } from "../sheet/suggest.js";

const wasm = createRequire(import.meta.url).resolve("@ironcalc/wasm-xlsx");
const binary = readFileSync(wasm.replace(/wasm\.js$/, "wasm_bg.wasm"));
const engine = await import(wasm);
engine.initSync({ module: binary });

const listed = FUNCTIONS.trim()
  .split("\n")
  .map((line) => {
    const [, name, args] = /^([A-Z][A-Z0-9.]*)\((.*)\) \S/.exec(line) ?? [];
    assert.ok(name, `${line} is a name, its arguments and what it gives`);
    return { name, args: args ? args.split(", ") : [] };
  });
// What each formula comes to, alone in a workbook: a call the engine cannot
// take must not spoil the next.
const shown = (formulas) =>
  formulas.map((formula) => {
    const model = new engine.Model("Sheet", "en", "UTC", "en");
    try {
      model.setUserInput(0, 1, 1, formula);
      return model.getFormattedCellValue(0, 1, 1);
    } finally {
      model.free();
    }
  });

test("the functions offered are those the engine calculates, all of them", () => {
  // A name the engine knows is written in its binary: every run of
  // capitals there, cut every way, is asked.
  const names = new Set();
  for (const run of binary.toString("latin1").match(/[A-Z][A-Z0-9.]+/g))
    for (let from = 0; from < run.length; from++)
      for (let to = from + 1; to <= Math.min(run.length, from + 26); to++)
        if (/[A-Z]/.test(run[from])) names.add(run.slice(from, to));
  const model = new engine.Model("Sheet", "en", "UTC", "en");
  const asked = [...names];
  model.pauseEvaluation();
  asked.forEach((name, at) => model.setUserInput(0, at + 1, 1, `=${name}()`));
  model.resumeEvaluation();
  model.evaluate();
  // TRUE and FALSE are written without brackets.
  const calculated = asked.filter((name, at) => {
    const answer = model.getFormattedCellValue(0, at + 1, 1);
    return (
      answer !== "#NAME?" &&
      answer !== "#N/IMPL!" &&
      !/^(TRUE|FALSE)$/.test(name)
    );
  });
  assert.deepEqual(
    listed.map(({ name }) => name),
    calculated.sort(),
  );
});

test("the engine takes every count of arguments a function is shown with", () => {
  const calls = listed.flatMap(({ name, args }) => {
    const all = args.filter((arg) => arg !== "…");
    const needed = all.filter((arg) => !arg.startsWith("[")).length;
    return Array.from({ length: all.length - needed + 1 }, (_, more) => ({
      name,
      count: needed + more,
    }));
  });
  // A wrong count is #ERROR! whatever the arguments are; the right one may
  // be refused for what they are, so each kind is tried.
  const kinds = ["1", '"a"', "A2:A4", "LAMBDA(x,x)"];
  const refused = calls.filter(({ name, count }) =>
    shown(
      kinds.map((kind) => `=${name}(${Array(count).fill(kind).join(",")})`),
    ).every((answer) => answer === "#ERROR!"),
  );
  assert.deepEqual(refused, []);
});

const at = (text) => suggestions(text, engine.getTokens);
const names = (text) => at(text).options?.map(({ name }) => name);

test("letters offer the functions that start with them, then those that hold them", () => {
  assert.deepEqual(names("=VLOOK"), ["VLOOKUP"]);
  assert.deepEqual(names("=lookup"), [
    "LOOKUP",
    "HLOOKUP",
    "VLOOKUP",
    "XLOOKUP",
  ]);
  assert.deepEqual(names("=SUMI"), ["SUMIF", "SUMIFS"]);
  assert.deepEqual(at("=SUMI").typed, 4);
  assert.deepEqual(at("=VLOOK").options[0], {
    name: "VLOOKUP",
    about:
      "Looks down the first column and gives the value beside, from a column",
  });
  // One letter is too little to look inside names for.
  assert.ok(names("=X").every((name) => name.startsWith("X")));
  // After an operator, an open bracket and a comma too.
  for (const text of ["=1+SUMI", "=A1*(SUMI", "=IF(A1,SUMI", "=-SUMI"])
    assert.deepEqual(names(text), ["SUMIF", "SUMIFS"], text);
});

test("nothing is offered where no function can be meant", () => {
  for (const text of [
    "",
    "SUM",
    "=",
    "=1+",
    // A cell, a number's exponent, a range's end, another tab's cell.
    "=A1",
    "=1e",
    "=LOG1",
    "=A1:B",
    "=Tasks!A",
    // Text, closed or not.
    '="SUM',
    '="a"',
    // A name no function has.
    "=QQQ",
  ])
    assert.deepEqual(at(text), {}, text);
});

test("inside a call its arguments are said, with the one the caret is at", () => {
  const hint = (text) => {
    const { name, args, at: reached } = at(text).hint ?? {};
    return name && `${name} ${args[reached]}`;
  };
  assert.deepEqual(at("=ROUND(").hint, {
    name: "ROUND",
    args: ["number", "num_digits"],
    at: 0,
  });
  assert.equal(hint("=round(A1,"), "ROUND num_digits");
  // Past its last argument none is the one.
  assert.equal(hint("=ROUND(A1,2,"), "ROUND undefined");
  // Where more may follow, the last stands for them.
  assert.equal(hint("=SUM(1,"), "SUM [number2]");
  assert.equal(hint("=SUM(1,2,3,4,"), "SUM …");
  // The innermost call open at the caret; a comma in text, in brackets or
  // in braces is not the call's.
  assert.equal(hint("=IF(SUM(1,2"), "SUM [number2]");
  assert.equal(hint("=IF(SUM(1,2),"), "IF value_if_true");
  assert.equal(hint('=IF(A1="a,b",(1+2),'), "IF [value_if_false]");
  assert.equal(hint("=SUM({1,2;3,4},"), "SUM [number2]");
  assert.equal(hint('=LEFT("a,b'), "LEFT text");
  // A call that is closed says no more, nor one the engine does not know.
  assert.equal(hint("=SUM(1,2)+"), undefined);
  assert.equal(hint("=NOPE(1,"), undefined);
  // Letters with no function to them leave the call's arguments said.
  assert.equal(hint("=ROUND(QQQ"), "ROUND number");
});
