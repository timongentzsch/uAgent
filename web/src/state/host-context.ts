import { createContext } from "preact";
import { useContext } from "preact/hooks";
import type {
  Act,
  AppModal,
  ConnectionPhase,
  Draft,
  Pin,
  Report,
  Session,
  Snapshot,
} from "../shared/types.ts";
import type { ValueStore } from "./value-store.ts";
import type { SnapshotStore } from "./snapshot-store.ts";

// What every surface needs of the host. It changes only with the connection,
// so a row of the sidebar reading it renders no more often than its props.
export interface Host {
  online: boolean;
  connection: ConnectionPhase;
  report: Report;
  pins: Pin[];
  store: SnapshotStore;
  load: (id: string) => Promise<Snapshot>;
  refresh: () => Promise<void>;
  forget: (id: string) => void;
  setModal: (modal: AppModal | null) => void;
  showContext: (item?: Session) => void;
  // The largest upload the host accepts; unknown from a host that does not say.
  uploadBytes?: number;
}

// What the open conversation’s surfaces share. The composer subscribes to its draft.
export interface Conversation {
  drafts: ValueStore<Record<string, Draft>>;
  setDraft: (value: Draft) => void;
  submit: (event: Event, jump: () => void, queue?: boolean) => Promise<void>;
  act: Act;
  upload: (files: File[]) => Promise<boolean>;
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
