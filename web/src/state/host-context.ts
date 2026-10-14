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

// What every surface needs of the host and of the open conversation, so a
// surface reads it here instead of taking it from the one above. The shell
// keeps the value stable while none of it changes.
export interface Host {
  online: boolean;
  connection: ConnectionPhase;
  // Runs a command on the open conversation.
  act: Act;
  report: Report;
  store: SnapshotStore;
  load: (id: string) => Promise<Snapshot>;
  refresh: () => Promise<void>;
  forget: (id: string) => void;
  // Attaches files to the open conversation's draft.
  upload: (files: File[]) => Promise<boolean>;
  uploading: boolean;
  // A message or a new conversation is on its way.
  busy: boolean;
  following: boolean;
  setFollowing: (following: boolean) => void;
  setModal: (modal: AppModal | null) => void;
  // Opens the context the open conversation was last sent.
  showContext: () => void;
  zoom: number;
}

export const HostContext = createContext<Host | null>(null);

export function useHostContext() {
  const host = useContext(HostContext);
  if (!host) throw new Error("useHostContext needs a HostContext provider.");
  return host;
}
