import "./activity.css";
import {
  ConnectionStatus,
  StatusLed,
  type ConnectionPhase,
} from "../../shared/connection-status.tsx";
import { plural } from "../../shared/quantities.ts";
import type {
  Activity,
  Agent,
  Pending,
  Report,
  SessionRef,
} from "../../shared/types.ts";
import {
  ArrowDownToLine,
  Bot,
  ChevronUp,
  Layers,
  Square,
  Terminal,
} from "lucide-preact";
import { cleanText, Button, IconButton, DataText } from "../../shared/ui.tsx";
import { Popover } from "../../shared/popover.tsx";
import { command } from "../../state/api.ts";
import { duration } from "../../shared/duration.ts";
import type { InspectorTarget } from "./inspector.tsx";
export interface ActivityProps {
  items?: Activity[];
  agents?: Agent[];
  session: SessionRef;
  online: boolean;
  report: Report;
  open: (target: InspectorTarget) => void;
}
export const active = (item: Activity) =>
  ["running", "starting", "stopping", "finishing"].includes(item.status || "");
// Supervised activities plus child agents not already listed as one.
export function withAgents(items: Activity[], agents: Agent[]): Activity[] {
  return [
    ...items,
    ...agents
      .filter((child) => !items.some((item) => item.agent_id === child.id))
      .map((child) => ({
        ...child,
        id: undefined,
        agent_id: child.id,
        kind: "agent",
        status: child.status || "idle",
      })),
  ];
}

export function activityLabel(items: Activity[] = []) {
  const running = items.filter(active);
  const agents = running.filter((item) => item.kind === "agent").length;
  const commands = running.length - agents;
  return [
    agents && plural(agents, "agent"),
    commands && plural(commands, "command"),
  ]
    .filter(Boolean)
    .join(" · ");
}
export function ActivityStatus({
  phase = "Ready",
  running,
  items = [],
  agents = [],
  pending,
  announce = false,
  present = false,
  connection,
}: {
  phase?: string;
  running?: boolean;
  items?: Activity[];
  agents?: Agent[];
  pending?: Pending | boolean | null;
  announce?: boolean;
  present?: boolean;
  connection?: ConnectionPhase;
}) {
  if (connection && connection !== "connected")
    return <ConnectionStatus phase={connection} />;
  const counts = activityLabel(withAgents(items, agents));
  return (
    <span
      class="activity-status"
      role={announce ? "status" : undefined}
      aria-atomic={announce ? "true" : undefined}
    >
      <StatusLed
        state={running && !pending ? "running" : present ? "active" : "idle"}
      />
      <span
        class="activity-caption"
        title={pending ? "Needs your input" : phase}
      >
        {pending ? "Needs your input" : phase}
      </span>
      {counts && <span class="activity-counts"> · {counts}</span>}
    </span>
  );
}
// The state above the input: counts live on ActivityButton below it.
export default function Activities({
  phase,
  running,
  pending,
  present,
  connection,
}: {
  running?: boolean;
  phase?: string;
  pending?: Pending | null;
  present?: boolean;
  connection?: ConnectionPhase;
}) {
  return (
    <div class="activities">
      <div class="status-line">
        <span class="activity-toggle">
          <ActivityStatus
            phase={phase}
            running={running}
            pending={pending}
            present={present}
            connection={connection}
            announce
          />
        </span>
      </div>
    </div>
  );
}

// Always under the input: what is running, then idle agents -- the same set
// as /agents -- since those stay resumable. Finished commands live only in the
// conversation.
export function ActivityButton({ open, ...props }: ActivityProps) {
  const now = (props.items || []).filter(active);
  const rows = [
    ...now,
    ...withAgents([], props.agents || []).filter(
      (agent) => !now.some((item) => item.agent_id === agent.agent_id),
    ),
  ];
  const counts = activityLabel(rows);
  return (
    <Popover
      label="Activity"
      side="top"
      align="start"
      buttonClass={`quiet activity-button${counts ? "" : " idle"}`}
      panelClass="activity-popover"
      trigger={
        <>
          <Layers aria-hidden="true" />
          {counts ? (
            <span>
              <DataText>{counts}</DataText>
            </span>
          ) : (
            <span>
              <span class="long">
                <DataText>No background work</DataText>
              </span>
              <span class="short">
                <DataText>Idle</DataText>
              </span>
            </span>
          )}
          <ChevronUp aria-hidden="true" />
        </>
      }
    >
      {(close) =>
        rows.length ? (
          <ul class="activity-list">
            {rows.map((item) => (
              <ActivityRow
                key={String(item.id ?? item.agent_id ?? item.label)}
                item={item}
                session={props.session}
                online={props.online}
                report={props.report}
                open={() => {
                  close();
                  open({ item });
                }}
              />
            ))}
          </ul>
        ) : (
          <p class="muted small">
            Nothing running. Background commands and agents appear here.
          </p>
        )
      }
    </Popover>
  );
}

function ActivityRow({
  item,
  session,
  online,
  report,
  open,
}: {
  item: Activity;
  session: SessionRef;
  online: boolean;
  report: Report;
  open: () => void;
}) {
  const name =
    item.kind === "agent" ? item.name || item.label : item.label || "Command";
  const act = (operation: "stop" | "background") =>
    command("activity", session, {
      operation,
      activity_id: item.id || 0,
      agent_id: item.agent_id || "",
      text: "",
    }).catch(report);
  return (
    <li class="activity-row">
      <Button
        variant="quiet"
        class="activity-open"
        disabled={!online}
        onClick={open}
      >
        {item.kind === "agent" ? <Bot /> : <Terminal />}
        <span class="activity-text">
          <strong>{cleanText(name || "")}</strong>
          <small>
            {item.status}
            {item.started_ms &&
              active(item) &&
              ` · ${duration(Math.max(0, Date.now() - item.started_ms))}`}
            {item.progress && ` · ${cleanText(item.progress)}`}
          </small>
        </span>
      </Button>
      {active(item) && item.kind !== "agent" && item.detached === false && (
        <IconButton
          label="Move to background"
          disabled={!online}
          onClick={() => act("background")}
        >
          <ArrowDownToLine />
        </IconButton>
      )}
      {active(item) && (
        <IconButton
          label={`Stop ${name}`}
          disabled={!online || item.status === "stopping"}
          onClick={() => act("stop")}
        >
          <Square />
        </IconButton>
      )}
    </li>
  );
}
