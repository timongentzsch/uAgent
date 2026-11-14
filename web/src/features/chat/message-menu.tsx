import type { Exchange, Node } from "../../shared/types.ts";
import { Menu, MenuItem } from "../../shared/menu.tsx";

// Items hide when their handler is missing.
export function MessageMenu({
  label,
  node,
  reply,
  statistics,
  http,
  branch,
}: {
  label: string;
  node: Node;
  reply?: Node;
  statistics?: (node: Node) => void;
  http?: (exchanges: Exchange[]) => void;
  branch?: (node: Pick<Node, "id">, edit: boolean) => void;
}) {
  const exchanges = reply?.http || node.http;
  const showHttp = !!http && (!!reply || (exchanges?.length || 0) > 0);
  if (!statistics && !showHttp && !branch) return null;
  return (
    <Menu label={label}>
      {branch && (
        <>
          <MenuItem onClick={() => branch(node, true)}>Edit from here</MenuItem>
          <MenuItem onClick={() => branch(node, false)}>
            Fork from here
          </MenuItem>
        </>
      )}
      {statistics && (
        <MenuItem onClick={() => statistics(node)}>Statistics</MenuItem>
      )}
      {reply && statistics && (
        <MenuItem onClick={() => statistics(reply)}>
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
