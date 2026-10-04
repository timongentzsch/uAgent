import { createContext } from "preact";
import { useContext } from "preact/hooks";
import type { Dispatch, StateUpdater, MutableRef } from "preact/hooks";
import type {
  Catalogue,
  InstallPrompt,
  Session,
  Snapshot,
} from "../../shared/types.ts";
import type { TimePrefs } from "../../shared/time.ts";

// What Settings is opened with. Panes read what they need from here, so the
// shell passes nothing through.
export interface SettingsProps {
  theme: string;
  setTheme: Dispatch<StateUpdater<string>>;
  motion: string;
  setMotion: Dispatch<StateUpdater<string>>;
  timePrefs: TimePrefs;
  setTimePrefs: Dispatch<StateUpdater<TimePrefs>>;
  zoom: number;
  setZoom: Dispatch<StateUpdater<number>>;
  // This device's verbosity level; empty follows the global setting.
  deviceDetail: string;
  setDeviceDetail: Dispatch<StateUpdater<string>>;
  installed: boolean;
  install: InstallPrompt | null;
  setInstall: Dispatch<StateUpdater<InstallPrompt | null>>;
  ios: boolean;
  update: ServiceWorker | null;
  // Unsent drafts or a waiting decision hold the reload.
  updateBlocked: boolean;
  snapshots: Record<string, Snapshot>;
  catalogue: Catalogue;
  online: boolean;
  notificationMode: boolean;
  setNotificationMode: Dispatch<StateUpdater<boolean>>;
  notifications: MutableRef<boolean>;
  refresh: () => Promise<void>;
  selected: string;
  session?: Session;
  logout: () => Promise<void>;
  instructions: () => void;
  // The open conversation's tools, in their own sheet.
  tools: () => void;
  initialSection?: string;
}

export const SettingsContext = createContext<SettingsProps | null>(null);
export const useSettings = () => useContext(SettingsContext)!;
