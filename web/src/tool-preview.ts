// Common tool first-preview: one abstraction for folded rows.
// Prefers native activity.label; else synthesizes from name+arguments keys
// only (never sniffs output). Mirrors backend ToolSummary in registry.cc.
import type { Block } from "./types.ts";
import { cleanText, statusLine, stringifyArgs } from "./display.ts";

function arg(args: unknown, key: string): string {
  if (typeof args === "object" && args !== null) {
    const value = (args as Record<string, unknown>)[key];
    if (typeof value === "string") return value;
    if (typeof value === "number") return String(value);
  }
  return "";
}

function firstLine(text = ""): string {
  return cleanText(text).split("\n")[0].slice(0, 120);
}

export function getToolPreview(block: Block): {
  title: string;
  subtitle: string;
  preview: string;
} {
  const name = block.name || "tool";
  const subtitle = statusLine({
    category: block.activity?.category,
    status: block.status,
    duration_ms: block.duration_ms,
  });
  const native = block.activity?.label?.trim();
  if (native) {
    const output = typeof block.text === "string" ? firstLine(block.text) : "";
    return { title: native, subtitle, preview: output };
  }
  const args = block.arguments;
  let title = name;
  const path = arg(args, "path") || arg(args, "file") || arg(args, "dir");
  switch (name) {
    case "read_path":
      title = path ? `Read ${path}` : "Read file";
      break;
    case "write_file":
      title = path ? `Write ${path}` : "Write file";
      break;
    case "edit_file":
      title = path ? `Edit ${path}` : "Edit file";
      break;
    case "grep":
      title = arg(args, "pattern")
        ? `Search ${arg(args, "pattern")}${path ? ` in ${path}` : ""}`
        : "Search";
      break;
    case "run":
    case "scratch": {
      const cmd = firstLine(
        arg(args, "command") || stringifyArgs(args),
      );
      title = cmd ? `$ ${cmd}` : name;
      break;
    }
    case "activity":
      title = arg(args, "operation")
        ? `Activity ${arg(args, "operation")}`
        : "Activity";
      break;
    case "memory":
      title = arg(args, "action")
        ? `Memory ${arg(args, "action")}`
        : "Memory";
      break;
    case "web_fetch":
      title = arg(args, "url") ? `Fetch ${arg(args, "url")}` : "Fetch URL";
      break;
    case "web_search":
      title = arg(args, "query")
        ? `Search ${arg(args, "query")}`
        : "Web search";
      break;
    default:
      if (path) title = `${name} ${path}`;
      break;
  }
  const output = typeof block.text === "string" ? firstLine(block.text) : "";
  return { title, subtitle, preview: output };
}
