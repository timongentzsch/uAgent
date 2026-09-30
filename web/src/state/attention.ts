import type { Session } from "../shared/types.ts";
import { folderOf } from "../shared/folder-label.tsx";

// A folder's threads: its sessions other than the coordinator.
export const threadsOf = (sessions: Session[], folder: string) =>
  sessions.filter(
    (item) => item.kind !== "coordinator" && folderOf(item) === folder,
  );

// Sessions waiting on a person's decision, in one folder when given.
export const waiting = (sessions: Session[], folder?: string) =>
  sessions.filter(
    (item) =>
      item.pending && (folder === undefined || folderOf(item) === folder),
  );
