// One adapter for every tool row. The native view names the call ("Editing
// a.ts" while running, "Edited a.ts" after); rows saved without one fall back
// to the native activity label, else the tool name. The subtitle is the diff
// stat, then status and duration.
import type { ToolRowBlock } from "../../shared/types.ts";
import { diffCounts, formatStat, statusLine } from "../../shared/display.ts";

export interface ToolRow {
  title: string;
  subtitle: string;
}

const diffStat = (change = "") => formatStat(diffCounts(change));

export function getToolRow(block: ToolRowBlock, running = false): ToolRow {
  const call = block.kind === "tool_result" ? block : undefined;
  const verb = block.view?.verb;
  const target = (block.view?.target || "").split("\n")[0];
  const title = verb
    ? `${verb[running ? 0 : 1]}${target ? ` ${target}` : ""}`
    : block.activity?.label?.trim() || call?.name || "tool";
  const subtitle = [
    diffStat(call?.change),
    statusLine({ status: block.status, duration_ms: call?.duration_ms }),
  ]
    .filter(Boolean)
    .join(" · ");
  return { title, subtitle };
}
