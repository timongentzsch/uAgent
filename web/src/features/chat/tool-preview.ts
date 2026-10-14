// One adapter for every tool row. The native view names the call ("Editing
// a.ts" while running, "Edited a.ts" after); rows saved without one fall back
// to the native activity label, else the tool name. The subtitle is the diff
// stat, then status and duration.
import type { Block } from "../../shared/types.ts";
import { diffCounts, formatStat, statusLine } from "../../shared/display.ts";

export interface ToolRow {
  title: string;
  subtitle: string;
}

const diffStat = (change = "") => formatStat(diffCounts(change));

export function getToolRow(block: Block, running = false): ToolRow {
  const verb = block.view?.verb;
  const target = (block.view?.target || "").split("\n")[0];
  const title = verb
    ? `${verb[running ? 0 : 1]}${target ? ` ${target}` : ""}`
    : block.activity?.label?.trim() || block.name || "tool";
  const subtitle = [
    diffStat(block.change),
    statusLine({ status: block.status, duration_ms: block.duration_ms }),
  ]
    .filter(Boolean)
    .join(" · ");
  return { title, subtitle };
}
