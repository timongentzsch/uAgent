import type { Block, PresentedBlock } from "./types.ts";

// One header per step: user turns always open, agent rows only flag the
// turn's first row. Bodies that follow share the step, not the chrome.
export function showsHeader(block: PresentedBlock): boolean {
  const userOwned =
    block.kind === "user" ||
    (block.kind === "attachment" && block.origin !== "tool");
  return userOwned || !!block.firstOfTurn || block.turn_root === block.id;
}

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
      rows.push(row);
      if (tool.id) calls.set(tool.id, row);
    }
  }
  // Membership and labels are native facts. This only nests adjacent rows for
  // disclosure; it never infers intent from a command, tool name or output.
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
        group ? { ...row, key: `group-${group.id}`, children: [row] } : row,
      );
  }
  // One actor mark per turn: flag the first agent row after each user
  // turn (user uploads count as the user's side). Grouped exploration
  // carries the flag on the wrapper; its children never re-flag.
  let agentSeen = false;
  for (const row of presented) {
    // Tool-sourced attachments are agent-side (origin fact); real user
    // uploads stay the user's side and reset the turn.
    if (row.kind === "user" || row.kind === "attachment") {
      if (row.kind === "user" || row.origin !== "tool") {
        agentSeen = false;
        continue;
      }
    }
    if (
      row.kind === "turn_summary" ||
      row.kind === "compaction" ||
      row.kind === "activity" ||
      row.kind === "error"
    )
      continue;
    if (!agentSeen) {
      row.firstOfTurn = true;
      agentSeen = true;
    }
  }
  return presented;
}
