// Common tool first-preview: one abstraction for folded rows.
// Prefers native activity.label; else synthesizes from name+arguments keys
// only (never sniffs output). Mirrors backend ToolSummary in registry.cc.
import type { Block } from "../../shared/types.ts";
import { cleanText, statusLine, stringifyArgs } from "../../shared/display.ts";
import { bytes } from "../../shared/quantities.ts";

// Retained blocks carry arguments as a truncated JSON string while live
// blocks carry the parsed object. Accept both so history rows keep their
// specific titles after retention instead of degrading to the bare tool
// name (most visible on activity rows, which poll every few seconds).
function argsObject(args: unknown): Record<string, unknown> {
  if (typeof args === "object" && args !== null)
    return args as Record<string, unknown>;
  if (typeof args === "string") {
    try {
      const parsed: unknown = JSON.parse(args);
      if (typeof parsed === "object" && parsed !== null)
        return parsed as Record<string, unknown>;
    } catch {
      // Truncated or lexical JSON: fall through to no known keys.
    }
  }
  return {};
}

function arg(args: unknown, key: string): string {
  const value = argsObject(args)[key];
  if (typeof value === "string") return value;
  if (typeof value === "number") return String(value);
  return "";
}

function firstLine(text = ""): string {
  return cleanText(text).split("\n")[0].slice(0, 120);
}

export function getToolPreview(block: Block): {
  title: string;
  subtitle: string;
} {
  const name = block.name || "tool";
  const subtitle = statusLine({
    category: block.activity?.category,
    status: block.status,
    duration_ms: block.duration_ms,
  });
  const native = block.activity?.label?.trim();
  if (native) return { title: native, subtitle };
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
      const cmd = firstLine(arg(args, "command") || stringifyArgs(args));
      title = cmd ? `$ ${cmd}` : name;
      break;
    }
    case "activity": {
      // Mirrors backend ToolSummary in registry.cc (minus the wait≤
      // timeout suffix, which is call config rather than what it does).
      const operation = arg(args, "operation");
      const id = arg(args, "id");
      const target = id ? `activity ${id}` : "activities";
      if (operation === "list") title = "List activities";
      else if (operation === "wait") {
        const mode = arg(args, "mode") || "any";
        const ids = argsObject(args)["ids"];
        const which =
          Array.isArray(ids) && ids.length
            ? ids.map((entry) => String(entry)).join(", ")
            : "all";
        title = `Wait for ${mode} \u00b7 ${which}`;
      } else if (operation === "write") {
        const size = argsObject(args)["chars"];
        title = `Write ${typeof size === "string" ? bytes(size.length) : "?"} \u2192 ${target}`;
      } else if (operation === "resize") {
        title = `Resize ${arg(args, "rows") || "?"}\u00d7${arg(args, "cols") || "?"} \u2192 ${target}`;
      } else if (operation === "stop") title = `Stop ${target}`;
      else if (operation === "poll") {
        const until = firstLine(arg(args, "until"));
        title = until ? `Await ${until} \u00b7 ${target}` : `Poll ${target}`;
      } else title = operation ? `Activity ${operation}` : "Activity";
      break;
    }
    case "memory":
      title = arg(args, "action") ? `Memory ${arg(args, "action")}` : "Memory";
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
  return { title, subtitle };
}

export interface ToolRow {
  title: string;
  subtitle: string;
  diffOnly: boolean;
}

// Single GUI adapter for every tool row. The title is the native
// activity.label when the session recorded one; sessions saved before those
// facts fall back to the synthesis above. A file change whose diff is the
// whole story renders as the diff alone.
export function getToolRow(block: Block): ToolRow {
  const { title, subtitle } = getToolPreview(block);
  const diffOnly =
    (block.name === "write_file" ||
      block.name === "edit_file" ||
      block.name === "delete_file") &&
    !!block.change &&
    !/fail|error/i.test(block.status || "");
  return { title, subtitle, diffOnly };
}
