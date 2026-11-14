import { createContext } from "preact";
import { useContext } from "preact/hooks";
import type { ConnectionPhase } from "../shared/types.ts";
import type { HostState } from "./use-host.ts";
export interface SavedHost {
  id: string;
  address: string;
  phase: ConnectionPhase;
  error: string;
  round_trip_ms?: number | null;
  // The µAgent version it runs, the main's own, once connected; and whether
  // that is being copied there now.
  version?: string;
  installing?: boolean;
}
// Local or a remote host, with the part of the main's catalogue it runs.
export interface Source extends SavedHost {
  name: string;
  state: Pick<HostState, "listed" | "catalogue">;
}
export interface HostView {
  terminal?: string;
  create?: "conversation";
}
export interface HostDestination extends HostView {
  id: string;
  session: string;
  place: string;
}
interface Hosts {
  destination: HostDestination | null;
  sources: Source[];
  active: string;
  gatewayOnline: boolean;
  open: (id: string, session?: string, place?: string, view?: HostView) => void;
  action: (action: string, id?: string, address?: string) => Promise<string>;
}
export const HostsContext = createContext<Hosts | null>(null);
export function useHosts() {
  const hosts = useContext(HostsContext);
  if (!hosts) throw new Error("useHosts needs a HostsContext provider.");
  return hosts;
}
export const useMultipleHosts = () => useHosts().sources.length > 1;
