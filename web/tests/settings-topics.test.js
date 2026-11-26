// Settings' one table against the host registry it lists, and the search
// that reads it.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { TOPICS, parseAddress } from "../src/shared/settings-topics.ts";
import { findSettings, settingsEntries } from "../src/shared/settings-index.ts";

const header = readFileSync(
  new URL("../../include/core/config_registry.h", import.meta.url),
  "utf8",
);
const listed = (name) =>
  [
    ...header
      .slice(header.indexOf(name))
      .split("};")[0]
      .matchAll(/\{("[^"]*"(?:, "[^"]*")*)\}/g),
  ].map(([, fields]) => JSON.parse(`[${fields}]`));

test("the topics and groups are the host's, in its order", () => {
  const topics = listed("kConfigTopics[]");
  const groups = listed("kConfigGroups[]");
  assert.deepEqual(
    TOPICS.filter(({ id }) => id !== "conversation").map(({ id, label }) => [
      id,
      label,
    ]),
    topics,
  );
  // A group the table places is the host's, in that topic; one it does not
  // name still shows, at the end of its topic, so a new one needs no change
  // here.
  for (const topic of TOPICS)
    for (const part of topic.parts)
      if ("group" in part)
        assert.ok(
          groups.some(([id, owner]) => id === part.group && owner === topic.id),
          `${topic.id} places the host group ${part.group}`,
        );
});

test("an address names a topic and a row", () => {
  assert.deepEqual(parseAddress("approvals/rules"), {
    topic: "approvals",
    row: "rules",
  });
  assert.deepEqual(parseAddress("about/logs.level"), {
    topic: "about",
    row: "logs.level",
  });
  assert.deepEqual(parseAddress("models"), { topic: "models", row: undefined });
  assert.deepEqual(parseAddress("nowhere/rules"), {
    topic: undefined,
    row: undefined,
  });
});

test("search finds settings, panels and actions across topics", () => {
  const setting = (key, label, group, topic, purpose = "") => ({
    key,
    label,
    group,
    topic,
    purpose,
    name: `UAGENT_${key}`,
    description: "",
  });
  const entries = settingsEntries(
    [
      // In registry order: a whole-word match still comes first.
      setting("logs.keepDays", "Logs kept", "logs", "about"),
      setting("logs.level", "Log level", "logs", "about"),
      setting("model", "Model", "conversation", "models"),
    ],
    [{ id: "logs", label: "Logs", topic: "about" }],
    TOPICS.map(({ id }) => id).filter((id) => id !== "conversation"),
  );
  const found = findSettings(entries, "log");
  assert.deepEqual(
    found
      .slice(0, 2)
      .map(({ label, topic, address }) => [label, topic, address]),
    [
      ["Log level", "About and diagnostics", "about/logs.level"],
      ["Logs kept", "About and diagnostics", "about/logs.keepDays"],
    ],
  );
  assert.ok(
    found.some(({ address }) => address === "host/devices"),
    "Paired devices holds Log out this device",
  );
  assert.ok(!found.some(({ label }) => label === "Model"));
  assert.ok(
    !entries.some(({ address }) => address.startsWith("conversation")),
    "This conversation is listed only while one is open",
  );
  assert.deepEqual(findSettings(entries, " "), []);
});
