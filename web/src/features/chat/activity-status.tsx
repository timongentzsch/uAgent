import "./activity.css";
import {
  ConnectionStatus,
  StatusLed,
  type ConnectionPhase,
} from "../../shared/connection-status.tsx";
import { count } from "../../shared/quantities.ts";
import type {
  Activity,
  ActivityDetail,
  Collaborator,
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
import { cleanText, IconButton } from "../../shared/ui.tsx";
import { Popover } from "../../shared/popover.tsx";
import { command } from "../../state/api.ts";
import { duration } from "../../shared/duration.ts";
import type { InspectorTarget } from "./inspector.tsx";
export interface ActivityProps {
  items?: Activity[];
  collaborators?: Collaborator[];
  session: SessionRef;
  online: boolean;
  report: Report;
  open: (target: InspectorTarget) => void;
}
export const active = (item: Activity) =>
  ["running", "starting", "stopping", "finishing"].includes(item.status || "");
// Supervised activities plus collaborators not already listed as one.
export function withCollaborators(
  items: Activity[],
  collaborators: Collaborator[],
): Activity[] {
  return [
    ...items,
    ...collaborators
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
    agents && `${count(agents)} agent${agents === 1 ? "" : "s"}`,
    commands && `${count(commands)} command${commands === 1 ? "" : "s"}`,
  ]
    .filter(Boolean)
    .join(" · ");
}
export function ActivityStatus({
  phase = "Ready",
  running,
  items = [],
  collaborators = [],
  pending,
  announce = false,
  present = false,
  connection,
}: {
  phase?: string;
  running?: boolean;
  items?: Activity[];
  collaborators?: Collaborator[];
  pending?: Pending | boolean | null;
  announce?: boolean;
  present?: boolean;
  connection?: ConnectionPhase;
}) {
  if (connection && connection !== "connected")
    return <ConnectionStatus phase={connection} />;
  const counts = activityLabel(withCollaborators(items, collaborators));
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

// Always under the input: what is running and persistent sidekicks, then
// idle agents -- the same set as /agents -- since those stay resumable.
// Finished commands live only in the conversation.
export function ActivityButton({ open, ...props }: ActivityProps) {
  const now = withCollaborators(
    props.items || [],
    props.collaborators || [],
  ).filter(
    (item) => active(item) || (item as ActivityDetail).persistent === true,
  );
  const rows = [
    ...now,
    ...withCollaborators([], props.collaborators || []).filter(
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
            <span>{counts}</span>
          ) : (
            <span>
              <span class="long">No background work</span>
              <span class="short">Idle</span>
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
            Nothing running. Background commands and sidekicks appear here.
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
      <button
        type="button"
        class="quiet activity-open"
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
      </button>
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
