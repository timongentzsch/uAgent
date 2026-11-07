import { createContext } from "preact";
import type {
  ActivityTarget,
  Exchange,
  FileTarget,
  Node,
  Report,
} from "../../shared/types.ts";

// What a transcript node can ask of the surface showing it: the
// conversation's handlers, or none of them for a side answer. Nodes read
// them from context, so their equality check covers only node data.
export interface MessageActions {
  report: Report;
  read?: (
    id: string,
    raw: boolean,
    signal?: AbortSignal,
  ) => Promise<{ text: string }>;
  inspect?: (id: string) => void;
  statistics?: (node: Node) => void;
  activity?: (target: ActivityTarget) => void;
  // Opens a conversation: an agent a row links to.
  open?: (id: string) => void;
  // Opens a cited file in the Files pane; none where there is no folder.
  openFile?: (target: FileTarget) => void;
  recall?: (node: Node) => Promise<void>;
  insert?: (node: Node) => Promise<void>;
  // Sends a message that failed again.
  retry?: (node: Node) => void;
  // Continues a stopped turn.
  resume?: () => void;
  branch?: (node: Pick<Node, "id">, edit: boolean) => void;
  http?: (exchanges: Exchange[]) => void;
}

export const MessageActions = createContext<MessageActions>({
  report: () => {},
});
