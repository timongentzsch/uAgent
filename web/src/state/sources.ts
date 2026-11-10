import { createContext } from "preact";
import { useContext } from "preact/hooks";
import type { HostState } from "./use-host.ts";
export interface SavedHost {
  id: string;
  name: string;
  address: string;
  phase: "connected" | "connecting" | "reconnecting" | "disconnected";
  error: string;
  round_trip_ms?: number | null;
}
export interface Source extends SavedHost {
  state?: HostState;
}
export interface HostDestination {
  id: string;
  session: string;
  place: string;
  terminal?: string;
  settings?: string;
  create?: "conversation" | "folder";
}
interface Hosts {
  destination: HostDestination | null;
  sources: Source[];
  active: string;
  gatewayOnline: boolean;
  open: (
    id: string,
    session?: string,
    place?: string,
    view?: {
      terminal?: string;
      settings?: string;
      create?: "conversation" | "folder";
    },
  ) => void;
  action: (action: string, id?: string, address?: string) => Promise<string>;
}
export const HostsContext = createContext<Hosts | null>(null);
export const useHosts = () => useContext(HostsContext);
export const useMultipleHosts = () => (useHosts()?.sources.length || 0) > 1;
