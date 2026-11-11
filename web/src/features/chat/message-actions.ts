import { createContext } from "preact";
import type {
  ActivityTarget,
  Exchange,
  FileTarget,
  Node,
  Report,
} from "../../shared/types.ts";

// Nodes read these from context, so their equality check covers only node
// data. A side answer provides none of them.
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
  open?: (id: string) => void;
  openFile?: (target: FileTarget) => void;
  recall?: (node: Node) => Promise<void>;
  insert?: (node: Node) => Promise<void>;
  retry?: (node: Node) => void;
  resume?: () => void;
  branch?: (node: Pick<Node, "id">, edit: boolean) => void;
  http?: (exchanges: Exchange[]) => void;
}

export const MessageActions = createContext<MessageActions>({
  report: () => {},
});
