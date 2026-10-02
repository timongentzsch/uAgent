import { test } from "node:test";
import assert from "node:assert/strict";
import {
  slashMatches,
  slashCompletion,
  parseSlash,
  fuzzy,
  fuzzyScore,
} from "../src/features/composer/slash.ts";
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
test("fuzzy matching ranks a prefix, then a word start, then a subsequence", () => {
  const labels = ["Open settings", "/status", "Session: stats probe", "Tools"];
  assert.deepEqual(
    fuzzy(labels, "st", (label) => label),
    ["/status", "Session: stats probe", "Open settings"],
  );
  assert.deepEqual(
    fuzzy(labels, "tls", (label) => label),
    ["Tools"],
  );
  assert.equal(fuzzyScore("xyz", "Tools"), -1);
  assert.equal(fuzzyScore("", "Tools"), 0);
});
