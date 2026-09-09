import type { Block, PresentedBlock } from "./types.ts";

// Join each output to the nearest preceding call within its request. Keep
// incomplete, live and orphaned output visible without changing native history.
export function presentMessages(blocks: Block[]): PresentedBlock[] {
  const rows: PresentedBlock[] = [];
  const calls = new Map<string, PresentedBlock>();
  for (const block of blocks) {
    if (block.kind === "user") calls.clear();
    if (block.kind === "tool_result") {
      const call = block.call_id && calls.get(block.call_id);
      if (call) {
        Object.assign(call, block, { result_loaded: true });
        continue;
      }
    }
    const tools = block.tools || [];
    if (!tools.length || block.text || block.reasoning || block.files?.length)
      rows.push(tools.length ? { ...block, tools: [] } : { ...block });
    for (const tool of tools) {
      const row: PresentedBlock = {
        kind: "tool_result",
        id: `t-${tool.id}`,
        key: `call-${block.id}-${tool.id}`,
        call_id: tool.id,
        name: tool.name,
        arguments: tool.arguments,
        status: tool.status,
        time: block.time,
        turn_root: block.turn_root,
        reply_to: block.reply_to,
        reply_excerpt: block.reply_excerpt,
        source: block,
        result_loaded: false,
      };
      rows.push(row);
      if (tool.id) calls.set(tool.id, row);
    }
  }
  return rows;
}
