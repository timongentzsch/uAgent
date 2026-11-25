import type { Session } from "../shared/types.ts";

export const waiting = (sessions: Session[]) =>
  sessions.filter((item) => item.needs_you);

// How many sessions need you: the tab title and the app badge show this
// number, and the sidebar's Needs you lists them.
export const needsYou = (sessions: Session[]) => waiting(sessions).length;

export const isRunning = (item: Session, online: boolean) =>
  online && !!item.turn_active;

// A conversation's side chat, until it is opened as a conversation of its
// own; an agent is the rest of what belongs to a conversation.
export const isSideChat = (item: Session) => !!item.side && !item.side.detached;
export const isAgent = (item: Session) => !!item.parent && !item.side;

export const sessionById = (sessions: Session[], id?: string) =>
  sessions.find((item) => item.id === id);

// Keep the catalogue's order when showing child sessions under their parent.
export function childrenByParent(sessions: Session[]) {
  const children = new Map<string, Session[]>();
  for (const session of sessions) {
    if (!session.parent) continue;
    const siblings = children.get(session.parent);
    if (siblings) siblings.push(session);
    else children.set(session.parent, [session]);
  }
  return children;
}

export type SessionState = "waiting" | "failed" | "working" | "idle";

export function statusOf(item: Session, online: boolean): SessionState {
  if (online && item.needs_you) return "waiting";
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

// The word for where a session is, wherever it is shown: an agent's row, the
// Agents panel.
export function statusWord(item: Session) {
  if (item.needs_you) return "Needs you";
  if (item.status === "waiting") return "Waiting";
  if (item.status === "starting") return "Starting";
  if (item.turn_active || item.status === "running") return "Working";
  if (item.status === "interrupted") return "Interrupted";
  if (item.error || item.status === "failed") return "Failed";
  return "Finished";
}
export const atWork = (item: Session) =>
  !["Interrupted", "Failed", "Finished"].includes(statusWord(item));

// Agents started by one call, newest group first. A group starts when its
// first agent's turn did, or, with none known, when it last changed.
export function agentGroups(agents: Session[]) {
  const groups = new Map<string, Session[]>();
  for (const agent of agents) {
    const call = agent.agent_call || agent.id;
    groups.set(call, [...(groups.get(call) || []), agent]);
  }
  return [...groups.values()]
    .map((group) => {
      const turns = group.map((a) => a.turn_started_ms).filter(Boolean);
      return {
        agents: group,
        at: turns.length
          ? Math.min(...(turns as number[]))
          : Math.max(...group.map((a) => a.updated || 0)),
      };
    })
    .sort((a, b) => b.at - a.at);
}
