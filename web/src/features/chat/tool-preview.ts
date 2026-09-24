// One adapter for every tool row: the native activity label is the title, the
// category, status and duration its subtitle. A file change whose diff is the
// whole story renders as the diff alone.
import type { Block } from "../../shared/types.ts";
import { statusLine } from "../../shared/display.ts";

export interface ToolRow {
  title: string;
  subtitle: string;
  diffOnly: boolean;
}

export function getToolRow(block: Block): ToolRow {
  const title = block.activity?.label?.trim() || block.name || "tool";
  const subtitle = statusLine({
    category: block.activity?.category,
    status: block.status,
    duration_ms: block.duration_ms,
  });
  const diffOnly =
    (block.name === "write_file" ||
      block.name === "edit_file" ||
      block.name === "delete_file") &&
    !!block.change &&
    !/fail|error/i.test(block.status || "");
  return { title, subtitle, diffOnly };
}
