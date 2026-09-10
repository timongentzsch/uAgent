import type { SlashCommand } from "./types.ts";

export function slashMatches(commands: SlashCommand[], text: string) {
  return /^\/[^\s]*$/.test(text)
    ? commands.filter((entry) => entry.command.startsWith(text))
    : [];
}

export function slashCompletion(matches: SlashCommand[]) {
  let prefix = matches[0]?.command || "";
  for (const entry of matches)
    while (!entry.command.startsWith(prefix)) prefix = prefix.slice(0, -1);
  return prefix + (matches.length === 1 && matches[0].argument ? " " : "");
}

export function parseSlash(commands: SlashCommand[], text: string) {
  const [name, argument = ""] = text.trim().split(/\s+(.*)/s);
  const spec = commands.find(
    (entry) => entry.command === name || entry.aliases.includes(name),
  );
  return { name: spec?.command || name, argument };
}
