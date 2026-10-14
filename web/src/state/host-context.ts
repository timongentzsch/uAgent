import { createContext } from "preact";
import { useContext } from "preact/hooks";
import type {
  Act,
  AppModal,
  ConnectionPhase,
  Report,
  Snapshot,
} from "../shared/types.ts";
import type { SnapshotStore } from "./snapshot-store.ts";

// What every surface needs of the host. It changes only with the connection,
// so a row of the sidebar reading it renders no more often than its props.
export interface Host {
  online: boolean;
  connection: ConnectionPhase;
  report: Report;
  store: SnapshotStore;
  load: (id: string) => Promise<Snapshot>;
  refresh: () => Promise<void>;
  forget: (id: string) => void;
  setModal: (modal: AppModal | null) => void;
  // Opens the context the open conversation was last sent.
  showContext: () => void;
}

// What the open conversation's surfaces share. It changes with every send
// and upload, so only the page and its composer read it, never a list row.
export interface Conversation {
  // Runs a command on the open conversation.
  act: Act;
  // Attaches files to the open conversation's draft.
  upload: (files: File[]) => Promise<boolean>;
  uploading: boolean;
  // A message or a new conversation is on its way.
  busy: boolean;
  following: boolean;
  setFollowing: (following: boolean) => void;
  zoom: number;
}

export const HostContext = createContext<Host | null>(null);
export const ConversationContext = createContext<Conversation | null>(null);

export function useHostContext() {
  const host = useContext(HostContext);
  if (!host) throw new Error("useHostContext needs a HostContext provider.");
  return host;
}

export function useConversation() {
  const conversation = useContext(ConversationContext);
  if (!conversation)
    throw new Error("useConversation needs a ConversationContext provider.");
  return conversation;
}
