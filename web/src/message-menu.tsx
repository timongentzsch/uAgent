import type { Exchange, PresentedBlock } from "./types.ts";
import { Menu, MenuItem } from "./popover.tsx";

// Single menu for every transcript row. Thinking and output tokens share
// the same surface; items hide only when their handler is missing.
export function MessageMenu({
  label,
  block,
  statistics,
  http,
}: {
  label: string;
  block: PresentedBlock;
  statistics?: (block: PresentedBlock) => void;
  http?: (exchanges: Exchange[]) => void;
}) {
  if (!statistics && !http) return null;
  const exchanges = block.http || block.source?.http;
  const showHttp =
    !!http &&
    (block.kind === "assistant" || (exchanges?.length || 0) > 0);
  return (
    <Menu label={label}>
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
