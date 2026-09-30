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

// What a session's row says about it, in words as well as a colour: it waits
// on you, it failed, it is working, or nothing worth a word.
export function statusOf(
  item: Session,
  online: boolean,
): { state: SessionState; label: string } {
  if (online && item.pending)
    return {
      state: "waiting",
      label: item.pending_prompt || "Needs your input",
    };
  if (item.error) return { state: "failed", label: "Needs attention" };
  if (online && item.turn_active)
    return {
      state: "working",
      label: item.activity || "Working",
    };
  return { state: "idle", label: "" };
}
