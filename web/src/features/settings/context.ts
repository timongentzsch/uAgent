import { createContext } from "preact";
import type { RefObject } from "preact";
import { useContext } from "preact/hooks";
import type { Dispatch, StateUpdater } from "preact/hooks";
import type {
  Catalogue,
  InstallPrompt,
  McpServer,
  Session,
} from "../../shared/types.ts";

// Panes read what they need from here, so the shell passes nothing through.
export interface SettingsProps {
  install: InstallPrompt | null;
  setInstall: Dispatch<StateUpdater<InstallPrompt | null>>;
  update: ServiceWorker | null;
  updateBlocked: boolean;
  mcp?: McpServer[];
  catalogue: Catalogue;
  online: boolean;
  notificationMode: boolean;
  setNotificationMode: Dispatch<StateUpdater<boolean>>;
  notifications: RefObject<boolean>;
  refresh: () => Promise<void>;
  selected: string;
  session?: Session;
  logout: () => Promise<void>;
  instructions: () => void;
  // Counts every saved change, whoever made it.
  version: number;
  initialSection?: string;
}

export const SettingsContext = createContext<SettingsProps | null>(null);
export const useSettings = () => useContext(SettingsContext)!;
