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
import { useRef } from "preact/hooks";
import {
  ArrowDownToLine,
  Bot,
  ChevronUp,
  Layers,
  Square,
  Terminal,
} from "lucide-preact";
import { cleanText, Button, IconButton, DataText } from "../../shared/ui.tsx";
import { SheetButton } from "../../shared/sheet.tsx";
import { command } from "../../state/api.ts";
import { duration } from "../../shared/duration.ts";
import type { InspectorTarget } from "./inspector.tsx";
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

function activityLabel(items: Activity[] = []) {
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
  pending,
  announce = false,
  present = false,
  connection,
}: {
  phase?: string;
  running?: boolean;
  items?: Activity[];
  pending?: Pending | boolean | null;
  announce?: boolean;
  present?: boolean;
  connection?: ConnectionPhase;
}) {
  // A listener hears a turn's edges, not every step between them: the
  // visible phase changes with each tool call, the announcement only when
  // a response starts, needs input or ends.
  const responded = useRef(false);
  if (running) responded.current = true;
  if (connection && connection !== "connected")
    return <ConnectionStatus phase={connection} />;
  const counts = activityLabel(items);
  const announcement = pending
    ? "Needs your input"
    : running
      ? "Responding"
      : responded.current
        ? "Response complete"
        : "";
  return (
    <span class="activity-status">
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
      {announce && (
        <span
          class="sr-only"
          role="status"
          aria-live="polite"
          aria-atomic="true"
        >
          {announcement}
        </span>
      )}
    </span>
  );
}

// Always under the input: what is running, then idle agents -- the same set
// as /agents -- since those stay resumable. Finished commands live only in the
// conversation.
export function ActivityButton({
  items,
  agents,
  session,
  online,
  report,
  open,
}: {
  items: Activity[];
  agents: Agent[];
  session: SessionRef;
  online: boolean;
  report: Report;
  open: (target: InspectorTarget) => void;
}) {
  const rows = withAgents(items.filter(active), agents);
  const counts = activityLabel(rows);
  return (
    <SheetButton
      label="Activity"
      buttonClass={`quiet activity-button${counts ? "" : " idle"}`}
      sheetClass="activity-sheet"
      trigger={
        <>
          <Layers />
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
          <ChevronUp />
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
                session={session}
                online={online}
                report={report}
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
    </SheetButton>
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
