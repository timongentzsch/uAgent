import { test } from "node:test";
import assert from "node:assert/strict";
import {
  slashMatches,
  slashCompletion,
  parseSlash,
} from "../src/features/composer/slash.ts";
import { fuzzy, fuzzyScore, matches } from "../src/shared/fuzzy.ts";
const commands = [
  { command: "/help", aliases: [], argument: "" },
  { command: "/quit", aliases: ["/exit"], argument: "" },
  { command: "/model", aliases: [], argument: "NAME" },
  { command: "/rename", aliases: [], argument: "TITLE" },
  { command: "/config", aliases: [], argument: "[KEY]" },
  { command: "/context", aliases: [], argument: "" },
  {
    command: "/clear",
    aliases: [],
    argument: "",
    client_only: true,
    terminal: true,
  },
];
test("slash completion uses the CLI common-prefix and argument rules", () => {
  assert.equal(slashCompletion(slashMatches(commands, "/co")), "/con");
  assert.equal(slashCompletion(slashMatches(commands, "/mo")), "/model ");
  assert.equal(slashCompletion(slashMatches(commands, "/h")), "/help");
  assert.equal(slashCompletion(slashMatches(commands, "/rename")), "/rename ");
  for (const text of ["hello /h", "/model name", "/help\nmore", ""])
    assert.deepEqual(slashMatches(commands, text), []);
  assert.deepEqual(parseSlash(commands, "/exit"), {
    name: "/quit",
    argument: "",
    clientOnly: false,
  });
  assert.deepEqual(parseSlash(commands, "/model vendor/name high"), {
    name: "/model",
    argument: "vendor/name high",
    clientOnly: false,
  });
});
test("a client-only command the page cannot run is not offered", () => {
  assert.deepEqual(slashMatches(commands, "/cl"), []);
  assert.equal(parseSlash(commands, "/clear").clientOnly, true);
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
test("a filter matches when every word occurs in some field", () => {
  assert.ok(matches("", "anything"));
  assert.ok(matches("  Web  PORT ", "UAGENT_WEB_PORT", "Web port"));
  assert.ok(matches("port web", "Web", undefined, "port"));
  assert.ok(!matches("web host", "Web port"));
});
