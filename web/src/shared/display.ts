// Pure functions only, no hooks or JSX, and TS-only imports, so node --test
// can load this module directly.
import { plural } from "./quantities.ts";
import type { Session, Statistics } from "./types.ts";

export const folderOf = (item: Session) => item.cwd || "";
export const projectsOf = (sessions: readonly Session[]) => [
  ...new Set(sessions.map(folderOf).filter(Boolean)),
];

export const permissionLabels = {
  ask: "Ask",
  auto: "Auto review",
  yolo: "YOLO",
  plan: "Plan",
} as const;
// What each mode lets through, in a line.
export const permissionNotes: Record<string, string> = {
  ask: "You approve each change, command and network call.",
  auto: "Edits in this folder run; a judge decides commands and the rest, and asks you when unsure.",
  yolo: "Everything runs, sandboxed. Folder rules that refuse or ask still hold.",
  plan: "Reads only, and writes the plan.",
};
// Any other mode asks.
export const permissionLabel = (mode?: string) =>
  permissionLabels[
    mode && mode in permissionLabels
      ? (mode as keyof typeof permissionLabels)
      : "ask"
  ];

export const capitalize = (text: string) =>
  text.charAt(0).toUpperCase() + text.slice(1);

export const cleanText = (text = "") =>
  text.replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, "");

export const formatStat = ([added, removed]: [number, number]) =>
  added || removed ? `+${added} −${removed}` : "";

// Git-style change line roles shared by tool changes (' '/'-'/'+'/'@'
// markers) and unified diffs ('---'/'+++'/'@@').
export function diffLineClass(line: string): string {
  if (line.startsWith("@@")) return "diff-hunk";
  if (line.startsWith("--- ") || line.startsWith("+++ ")) return "diff-head";
  const marker = line.charAt(0);
  if (marker === "+") return "diff-add";
  if (marker === "-") return "diff-del";
  if (marker === "@") return "diff-hunk";
  return "diff-ctx";
}

// A router such as OpenRouter picks the upstream per call; naming it is how
// a provider's fault is traced.
export const servedBy = (provider?: string) =>
  provider ? `served by ${provider}` : "";

// One row per upstream: the calls it served and the tool calls rejected from
// its responses.
export const servedByRows = (
  served: Statistics["served_by"] = {},
): [string, string][] =>
  Object.entries(served).map(([name, { model_calls = 0, rejected_calls }]) => [
    `Served by ${name}`,
    [
      plural(model_calls, "model call"),
      rejected_calls && plural(rejected_calls, "rejected tool call"),
    ]
      .filter(Boolean)
      .join(" · "),
  ]);

const agentTopic = /^([^:\n]{1,44}):(?:\s|$)/u;

export function agentAssignmentText(assignment: string) {
  return cleanText(assignment).trim().replace(agentTopic, "").trim();
}

// Assignment prompts often start with a topic ("fstab: validate syntax…").
// Keep the original text for inspection; the conversation control needs a name.
export function shortAgentName(assignment: string) {
  const first = cleanText(assignment).trim().split("\n", 1)[0] || "Agent";
  const topic = agentTopic.exec(first)?.[1];
  if (topic) return topic.trim();
  if (first.length <= 40) return first;
  const words = first
    .slice(0, 38)
    .replace(/\s+\S*$/, "")
    .trimEnd();
  return (words || first.slice(0, 38)) + "…";
}
