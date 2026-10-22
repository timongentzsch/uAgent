import { createContext } from "preact";
import { useContext } from "preact/hooks";
import type {
  Act,
  AppModal,
  ConnectionPhase,
  Draft,
  Report,
  Session,
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
  // Opens the context a conversation was last sent: the open one's, or the
  // given one's.
  showContext: (item?: Session) => void;
  // Starts a message to a member (or the moderator) in its panel's composer.
  askMember: (item: Session) => void;
}

// What the open conversation's surfaces share. It changes with every send,
// upload and key typed, so only the page, its composer and the dialogs read
// it, never a list row.
export interface Conversation {
  // The open conversation's draft, and sending it.
  draft: Draft;
  setDraft: (value: Draft) => void;
  submit: (event: Event, jump: () => void, queue?: boolean) => Promise<void>;
  // Runs a command on the open conversation.
  act: Act;
  // Attaches files to the open conversation's draft.
  upload: (files: File[]) => Promise<boolean>;
  uploading: boolean;
  // A message or a new conversation is on its way.
  busy: boolean;
  following: boolean;
  setFollowing: (following: boolean) => void;
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
