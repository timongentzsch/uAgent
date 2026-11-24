import { createContext } from "preact";
import { useContext, useMemo, useRef } from "preact/hooks";
import { statusOf } from "../../state/attention.ts";
import type { Node, Session } from "../../shared/types.ts";
import { Button, cleanText } from "../../shared/ui.tsx";
import { capitalize, shortAgentName } from "../../shared/display.ts";
import {
  agentBatch,
  agentLink,
  agentReport,
} from "../../shared/message-view.ts";
import { MessageActions } from "./message-actions.ts";

export const AgentParticipants = createContext({
  names: new Map<string, string>(),
  sessions: new Map<string, Session>(),
});

// A streamed token changes nodes, but only a changed assignment or child
// catalogue needs to update the bylines already on screen.
export function useParticipants(nodes: Node[], children?: Session[]) {
  const calls = useMemo(
    () =>
      nodes.filter(
        (node) =>
          node.agent_label ||
          (node.kind === "call" && node.parts?.some(agentLink)),
      ),
    [nodes],
  );
  const previous = useRef<Node[]>([]);
  if (
    calls.length !== previous.current.length ||
    calls.some((node, i) => node !== previous.current[i])
  )
    previous.current = calls;
  const assignments = previous.current;
  return useMemo(() => {
    const names = new Map<string, string>();
    for (const node of assignments) {
      const sender = node.agent_label && agentReport(node);
      if (sender && node.agent_label && !names.has(sender.session))
        names.set(sender.session, node.agent_label);
      for (const { link, assignment } of agentBatch(node)?.replies || [])
        if (link && assignment && assignment !== "Open agent")
          names.set(String(link.id), assignment);
    }
    return {
      names,
      sessions: new Map(children?.map((child) => [child.id, child])),
    };
  }, [assignments, children]);
}

const deliveryState: Record<string, string> = {
  failed: "Failed",
  cancelled: "Cancelled",
  interrupted: "Interrupted",
  working: "Started",
  "needs decision": "Decision requested",
};
const liveState = {
  working: "Working",
  failed: "Failed",
  waiting: "Review decision",
  idle: "",
};

// A delivery records what the child said then. The delegation can also show
// what that child needs now, even when its tool row is folded.
export function AgentSender({
  id,
  nameId,
  title,
  outcome,
  online,
  live = false,
  waitingOnly = false,
}: {
  id: string;
  nameId?: string;
  title: string;
  outcome?: string;
  online: boolean;
  live?: boolean;
  waitingOnly?: boolean;
}) {
  const { names, sessions } = useContext(AgentParticipants);
  const child = sessions.get(id);
  const assignment = names.get(id);
  const { open } = useContext(MessageActions);
  const now = live && child && statusOf(child, online);
  const waiting = now === "waiting";
  if (waitingOnly && !waiting) return null;
  const name = cleanText(
    assignment || (title === "Open agent" && child?.title) || title || "Agent",
  );
  const short = shortAgentName(name);
  const ended =
    live && outcome && outcome !== "working" && outcome !== "needs decision";
  const state =
    ended && !waiting
      ? capitalize(outcome)
      : (now && liveState[now]) || deliveryState[outcome || ""];
  const Name = nameId ? "h3" : "div";
  return (
    <div class="agent-sender small">
      <Name class="sender-heading">
        {open ? (
          <Button
            variant="quiet"
            class="sender-name"
            title={name}
            aria-label={
              waiting
                ? `Review decision · ${name}`
                : name === "Open agent"
                  ? name
                  : `Open agent · ${name}`
            }
            onClick={() => open(id)}
          >
            <span id={nameId}>{short}</span>
            {waiting && <span> · {state}</span>}
          </Button>
        ) : (
          <strong id={nameId} title={name}>
            {short}
          </strong>
        )}
      </Name>
      {state && !(waiting && open) && <span class="muted">{state}</span>}
    </div>
  );
}
