import type { Session } from "../shared/types.ts";

export const waiting = (sessions: Session[]) =>
  sessions.filter((item) => item.pending);

// How many sessions need you: the tab title and the app badge show this
// number, and the sidebar's Needs you lists them.
export const needsYou = (sessions: Session[]) => waiting(sessions).length;

export const isRunning = (item: Session, online: boolean) =>
  online && !!item.turn_active;

export const sessionById = (sessions: Session[], id?: string) =>
  sessions.find((item) => item.id === id);

export type SessionState = "waiting" | "failed" | "working" | "idle";

export function statusOf(item: Session, online: boolean): SessionState {
  if (online && item.pending) return "waiting";
  if (item.error) return "failed";
  return isRunning(item, online) ? "working" : "idle";
}

// What the list shows: a conversation nobody has said anything in is left
// out, unless it is the open one or something is happening in it; an agent
// too.
export const shownSessions = (
  sessions: Session[],
  selected: string,
  online = true,
) =>
  sessions.filter(
    (item) =>
      item.turns !== 0 ||
      item.id === selected ||
      statusOf(item, online) !== "idle",
  );
