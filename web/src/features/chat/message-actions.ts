import { createContext } from "preact";
import type {
  ActivityTarget,
  Block,
  Exchange,
  PresentedBlock,
  Report,
  Session,
  UserBlock,
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
  statistics?: (block: PresentedBlock) => void;
  activity?: (target: ActivityTarget) => void;
  recall?: (block: UserBlock) => void;
  // Sends a message that failed again.
  retry?: (block: UserBlock) => void;
  // Continues a stopped turn.
  resume?: () => void;
  branch?: (block: Pick<Block, "id">, edit: boolean) => void;
  http?: (exchanges: Exchange[]) => void;
  // In a panel's chat: its members, and a way to see the prompt a
  // participant was last sent (a member by name, the panel by none).
  team?: Session[];
  moderator?: string;
  prompt?: (member?: string) => void;
}

export const MessageActions = createContext<MessageActions>({
  report: () => {},
});
