import type { Exchange, PresentedBlock } from "../../shared/types.ts";
import { Menu, MenuItem } from "../../shared/popover.tsx";

// Single menu for every transcript row. Thinking and output tokens share
// the same surface; items hide only when their handler is missing.
export function MessageMenu({
  label,
  block,
  statistics,
  http,
  branch,
}: {
  label: string;
  block: PresentedBlock;
  statistics?: (block: PresentedBlock) => void;
  http?: (exchanges: Exchange[]) => void;
  // Your own messages: continue in a fork from here, editing this message
  // (edit) or keeping it and its reply.
  branch?: (block: PresentedBlock, edit: boolean) => void;
}) {
  if (!statistics && !http && !branch) return null;
  const exchanges = block.http || block.source?.http;
  const showHttp =
    !!http && (block.kind === "assistant" || (exchanges?.length || 0) > 0);
  return (
    <Menu label={label}>
      {branch && (
        <>
          <MenuItem onClick={() => branch(block, true)}>
            Edit from here
          </MenuItem>
          <MenuItem onClick={() => branch(block, false)}>
            Fork from here
          </MenuItem>
        </>
      )}
      {statistics && (
        <MenuItem onClick={() => statistics(block)}>Statistics</MenuItem>
      )}
      {block.source && statistics && (
        <MenuItem onClick={() => statistics(block.source!)}>
          Model call statistics
        </MenuItem>
      )}
      {showHttp && (
        <MenuItem onClick={() => http(exchanges || [])}>
          HTTP request/response
        </MenuItem>
      )}
    </Menu>
  );
}
