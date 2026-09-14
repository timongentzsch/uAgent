import type { Block, PresentedBlock } from "./types.ts";

// Flat, stable, uniform rows. Every row renders the same chrome, so there is
// no header/matrix state to derive (and nothing to drift). Only two things
// happen here:
// 1. tool results join their preceding call within the same request;
// 2. adjacent rows of one native exploration group nest for disclosure.
// Both preserve input objects and keep stable keys across streaming deltas.
export function presentMessages(blocks: Block[]): PresentedBlock[] {
  const rows: PresentedBlock[] = [];
  const calls = new Map<string, number>();
  const occurrence = (
    block: Pick<Block, "occurrence_id" | "response_id" | "call_id">,
  ) =>
    block.occurrence_id ||
    (block.call_id
      ? block.response_id
        ? `${block.response_id}:${block.call_id}`
        : `t-${block.call_id}`
      : "");
  for (const block of blocks) {
    if (block.kind === "user") calls.clear();
    if (block.kind === "tool_result") {
      const index = calls.get(occurrence(block));
      if (index !== undefined && rows[index]) {
        rows[index] = { ...rows[index], ...block, result_loaded: true };
        continue;
      }
    }
    const tools = block.tools || [];
    if (!tools.length || block.text || block.reasoning || block.files?.length)
      rows.push({
        ...block,
        key: block.response_id || block.id,
        ...(tools.length ? { tools: [] } : {}),
      });
    for (const tool of tools) {
      const callId = tool.call_id || tool.id || "unknown";
      const occurrenceId =
        tool.occurrence_id ||
        (tool.response_id || block.response_id
          ? `${tool.response_id || block.response_id}:${callId}`
          : `t-${callId}`);
      const row: PresentedBlock = {
        kind: "tool_result",
        id: occurrenceId,
        key: occurrenceId,
        call_id: callId,
        occurrence_id: occurrenceId,
        response_id: tool.response_id || block.response_id,
        detail_id: tool.detail_id,
        name: tool.name,
        activity: tool.activity,
        arguments: tool.arguments,
        status: tool.status,
        time: block.time,
        turn_root: block.turn_root,
        reply_to: block.reply_to,
        reply_excerpt: block.reply_excerpt,
        source: block,
        result_loaded: false,
      };
      calls.set(occurrenceId, rows.length);
      rows.push(row);
    }
  }
  // Membership and labels are native facts. This only nests adjacent rows for
  // disclosure; it never infers intent from a command, tool name or output.
  // Wrappers are new objects per call with stable keys; inputs are never mutated.
  const presented: PresentedBlock[] = [];
  for (const row of rows) {
    const group = row.activity?.group;
    const previous = presented.at(-1);
    if (
      group &&
      previous?.activity?.group?.id === group.id &&
      previous.children
    )
      previous.children.push(row);
    else
      presented.push(
        group
          ? {
              ...row,
              key: `group-${row.response_id || "historical"}-${group.id}`,
              children: [row],
            }
          : row,
      );
  }
  return presented;
}
