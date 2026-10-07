import { createContext } from "preact";
import type {
  Block,
  Exchange,
  PresentedBlock,
  Report,
  Session,
} from "../../shared/types.ts";

// What a transcript row can ask of the surface showing it: the
// conversation's handlers, or the inspector's for a subagent thread. Rows
// read them from context, so their equality check covers only row data.
export interface MessageActions {
  report: Report;
  read?: (
    id: string,
    raw: boolean,
    signal?: AbortSignal,
  ) => Promise<{ text: string }>;
  inspect?: (id: string) => void;
  statistics?: (block: Block) => void;
  activity?: (block: Block) => void;
  recall?: (block: PresentedBlock) => void;
  // Sends a message that failed again.
  retry?: (block: PresentedBlock) => void;
  // Continues a stopped turn.
  resume?: () => void;
  branch?: (block: PresentedBlock, edit: boolean) => void;
  http?: (exchanges: Exchange[]) => void;
  // In a coordinator's chat: its members, and a way to see the prompt a
  // participant was last sent (a member by name, the coordinator by none).
  team?: Session[];
  prompt?: (member?: string) => void;
}

export const MessageActions = createContext<MessageActions>({
  report: () => {},
});
