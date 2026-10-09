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

// How many sessions need you: the sidebar's inbox, the tab title and the
// app badge all show this one number.
export const needsYou = (sessions: Session[]) => waiting(sessions).length;

export type SessionState = "waiting" | "failed" | "working" | "idle";

// What a session's row shows of it: it waits on you, it failed, it is
// working, or nothing worth a mark.
export function statusOf(item: Session, online: boolean): SessionState {
  if (online && item.pending) return "waiting";
  if (item.error) return "failed";
  return online && item.turn_active ? "working" : "idle";
}
