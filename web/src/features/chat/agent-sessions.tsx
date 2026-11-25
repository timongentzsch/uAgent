import { createContext } from "preact";
import { useContext, useMemo, useRef } from "preact/hooks";
import { statusOf } from "../../state/attention.ts";
import { type AgentReports, agentReports } from "../../shared/message-view.ts";
import type { LinkPart, Node, Session } from "../../shared/types.ts";
import { Button } from "../../shared/ui.tsx";
import { agentName } from "../../shared/turn-status.tsx";
import { MessageActions } from "./message-actions.ts";

export interface Agents {
  // This conversation's agents and side chats, by id.
  sessions: Map<string, Session>;
  // The conversation that started this one, when it is an agent.
  parent?: string;
  reports: AgentReports;
}
export const AgentSessions = createContext<Agents>({
  sessions: new Map(),
  reports: {},
});

// What the loaded nodes say each agent last reported stays the same object
// until one changes: the composer renders when it does, not with every
// streamed token.
export function useAgents(
  list: Session[],
  parent: string | undefined,
  nodes: Node[],
): Agents {
  const held = useRef<AgentReports>({});
  const reports = agentReports(nodes);
  if (JSON.stringify(reports) !== JSON.stringify(held.current))
    held.current = reports;
  return useMemo(
    () => ({
      sessions: new Map(list.map((child) => [child.id, child])),
      parent,
      reports: held.current,
    }),
    [list, parent, held.current],
  );
}

// What a folded delegation shows of an agent waiting on a person.
export function ReviewDecision({
  link,
  online,
}: {
  link: LinkPart;
  online: boolean;
}) {
  const child = useContext(AgentSessions).sessions.get(String(link.id));
  const { open } = useContext(MessageActions);
  if (!child || !open || statusOf(child, online) !== "waiting") return null;
  const name = agentName(child);
  return (
    <Button
      variant="quiet"
      class="review-decision small"
      aria-label={`Review decision · ${name}`}
      onClick={() => open(child.id)}
    >
      {name} · Review decision
    </Button>
  );
}
