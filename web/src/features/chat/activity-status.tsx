import "./activity.css";
import {
  ConnectionStatus,
  StatusLed,
  type ConnectionPhase,
} from "../../shared/connection-status.tsx";
import { count } from "../../shared/quantities.ts";
import type { ComponentChildren } from "preact";
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
  ChevronDown,
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
// What is running and persistent sidekicks, then idle agents -- the same set
// as /agents -- since those stay resumable. Finished commands live only in
// the conversation.
export default function Activities({
  children,
  open,
  ...props
}: ActivityProps & {
  running?: boolean;
  phase?: string;
  pending?: Pending | null;
  present?: boolean;
  connection?: ConnectionPhase;
  children?: ComponentChildren;
}) {
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
  const status = <ActivityStatus {...props} announce />;
  return (
    <div class="activities">
      <div class="status-line">
        {rows.length ? (
          <Popover
            label="Activity"
            side="top"
            align="start"
            buttonClass="quiet activity-toggle"
            panelClass="activity-popover"
            trigger={
              <>
                {status}
                <ChevronDown />
              </>
            }
          >
            {(close) => (
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
            )}
          </Popover>
        ) : (
          <span class="activity-toggle">{status}</span>
        )}
        {children}
      </div>
    </div>
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
