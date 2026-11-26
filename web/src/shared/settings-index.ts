import { fuzzyScore, matches } from "./fuzzy.ts";
import { TOPICS, type Topic } from "./settings-topics.ts";
import type { ConfigGroup, ConfigSetting } from "./types.ts";

// One thing Settings holds, as search lists it: a topic, a panel or a host
// setting, with the address that opens it.
export interface SettingsEntry {
  address: string;
  label: string;
  // What it is for, and the topic it is under.
  detail: string;
  topic: string;
  words: string;
}

export function settingsEntries(
  settings: ConfigSetting[],
  groups: ConfigGroup[],
  topics: readonly Topic[],
): SettingsEntry[] {
  return TOPICS.filter(({ id }) => topics.includes(id)).flatMap((topic) => [
    {
      address: topic.id,
      label: topic.label,
      detail: "",
      topic: topic.label,
      words: "",
    },
    ...topic.parts.flatMap((part) =>
      "panel" in part
        ? [
            {
              address: `${topic.id}/${part.panel}`,
              label: part.label,
              detail: "",
              topic: topic.label,
              words: part.words,
            },
          ]
        : [],
    ),
    ...settings
      .filter((setting) => setting.topic === topic.id)
      .map((setting) => ({
        address: `${topic.id}/${setting.key}`,
        label: setting.label,
        detail: setting.purpose || setting.description,
        topic: topic.label,
        words: [
          setting.key,
          setting.name,
          setting.description,
          groups.find(({ id }) => id === setting.group)?.label,
        ].join(" "),
      })),
  ]);
}

// Every word of the query somewhere in an entry; names that start with it
// first.
export function findSettings(entries: SettingsEntry[], query: string) {
  if (!query.trim()) return [];
  const score = (entry: SettingsEntry) => {
    const named = fuzzyScore(query, entry.label);
    return named < 0 ? Infinity : named;
  };
  return entries
    .filter((entry) => matches(query, entry.label, entry.detail, entry.words))
    .map((entry, index) => ({ entry, index, rank: score(entry) }))
    .sort((a, b) => a.rank - b.rank || a.index - b.index)
    .map(({ entry }) => entry);
}
