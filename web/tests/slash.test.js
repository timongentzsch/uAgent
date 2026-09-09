import { test } from "node:test";
import assert from "node:assert/strict";
import { slashMatches, slashCompletion, parseSlash } from "../src/slash.ts";
const commands = [
  { command: "/help", aliases: ["/commands"], argument: "" },
  { command: "/model", aliases: [], argument: "NAME" },
  { command: "/models", aliases: [], argument: "[QUERY]" },
];
test("slash completion uses the CLI common-prefix and argument rules", () => {
  assert.equal(slashCompletion(slashMatches(commands, "/mo")), "/model");
  assert.equal(slashCompletion(slashMatches(commands, "/h")), "/help");
  assert.equal(slashCompletion(slashMatches(commands, "/models")), "/models ");
  for (const text of ["hello /h", "/model name", "/help\nmore", ""])
    assert.deepEqual(slashMatches(commands, text), []);
  assert.deepEqual(parseSlash(commands, "/commands"), {
    name: "/help",
    argument: "",
  });
  assert.deepEqual(parseSlash(commands, "/model vendor/name high"), {
    name: "/model",
    argument: "vendor/name high",
  });
});
