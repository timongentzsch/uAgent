import "./activity.css";
import { ConnectionStatus, StatusLed } from "./connection-status.tsx";
import { plural } from "./quantities.ts";
import type {
  Activity,
  ConnectionPhase,
  InspectorTarget,
  Report,
  Session,
  SessionRef,
} from "./types.ts";
import { useRef } from "preact/hooks";
import {
  ArrowDownToLine,
  ChevronUp,
  CircleAlert,
  Square,
  Terminal,
  Users,
} from "lucide-preact";
import {
  cleanText,
  Button,
  IconButton,
  DataText,
  updateChecked,
} from "./ui.tsx";
import { SheetButton } from "./sheet.tsx";
import { command } from "../state/api.ts";
import { useAction } from "./use-action.ts";
import { useElapsed } from "./time.ts";
import { focusDecision, revealLiveRow } from "./navigation.ts";
// The one control below the input for the turn and the work behind it.

export const active = (item: Activity) =>
  ["running", "starting", "stopping", "finishing"].includes(item.status || "");

// Whether the turn works, waits on you or stopped short, for how long, and
// what still runs behind it: "Working · 4m 02s · 1 command · 2 agents". It
// never names a step, which its own row shows, and is absent while nothing
// runs and nothing needs saying.
function ActivityButtonView({
  items,
  agents,
  session,
  online,
  connection,
  running,
  started,
  pending,
  stopped,
  resume,
  report,
  open,
  choose,
}: {
  items: Activity[];
  // The agents this conversation started that are working.
  agents: Session[];
  session: SessionRef;
  online: boolean;
  connection?: ConnectionPhase;
  running: boolean;
  // When the running turn started: its time so far.
  started?: number;
  // A decision the turn waits on.
  pending: boolean;
  // Why the last turn stopped short, until something new is sent.
  stopped?: string;
  resume?: () => void;
  report: Report;
  open: (target: InspectorTarget) => void;
  choose: (id: string) => void;
}) {
  // A listener hears a turn's edges: when a response starts, needs input
  // or ends.
  const responded = useRef(false);
  if (running) responded.current = true;
  const elapsed = useElapsed(running && !pending, started);
  const rows = items.filter(active);
  const counts = [
    rows.length ? plural(rows.length, "command") : "",
    agents.length ? plural(agents.length, "agent") : "",
  ].filter(Boolean);
  const announcement = pending
    ? "Needs your input"
    : running
      ? "Responding"
      : stopped || (responded.current ? "Response complete" : "");
  const announce = (
    <span class="sr-only" role="status" aria-live="polite" aria-atomic="true">
      {announcement}
    </span>
  );
  if (connection && connection !== "connected")
    return <ConnectionStatus phase={connection} />;
  if (pending || (!running && stopped))
    return (
      <span class="activity-state">
        {pending ? <CircleAlert /> : <StatusLed state="failed" />}
        <DataText>{pending ? "Needs you" : stopped}</DataText>
        <Button
          variant={pending ? "primary" : "secondary"}
          size="compact"
          disabled={!online}
          onClick={pending ? focusDecision : resume}
        >
          {pending ? "Show" : "Continue"}
        </Button>
        {announce}
      </span>
    );
  const details = [elapsed, ...counts].filter(Boolean).join(" · ");
  const summary = [running ? "Working" : "", details]
    .filter(Boolean)
    .join(" · ");
  if (!summary) return announce;
  return (
    <>
      <SheetButton
        label={summary}
        heading="Activity"
        buttonClass="activity-button"
        sheetClass="activity-sheet"
        trigger={
          <>
            <StatusLed state="running" />
            <span>
              <DataText>
                {running && <span class="shimmer-text">Working</span>}
                {running && details && " · "}
                {details}
              </DataText>
            </span>
            <ChevronUp />
          </>
        }
      >
        {(close) => (
          <ul class="activity-list divided">
            {running && (
              <li class="activity-row">
                <Button
                  variant="quiet"
                  class="activity-open"
                  onClick={() => {
                    close();
                    revealLiveRow();
                  }}
                >
                  <StatusLed state="running" />
                  <span class="activity-text">
                    <strong>This turn</strong>
                    <small>
                      {["Working", elapsed].filter(Boolean).join(" · ")}
                    </small>
                  </span>
                </Button>
              </li>
            )}
            {rows.map((item) => (
              <ActivityRow
                key={String(item.id ?? item.label)}
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
            {agents.map((agent) => (
              <AgentRow
                key={agent.id}
                agent={agent}
                online={online}
                report={report}
                open={() => {
                  close();
                  choose(agent.id);
                }}
              />
            ))}
          </ul>
        )}
      </SheetButton>
      {announce}
    </>
  );
}
// The composer renders with every key pressed; the control only when what
// it shows changes.
export const ActivityButton = updateChecked(ActivityButtonView);

// What a running activity can be told: Stop, and for a command the turn
// still waits on, Move to background.
export function ActivityControls({
  item,
  name,
  session,
  online,
  report,
}: {
  item: Activity;
  name: string;
  session: SessionRef;
  online: boolean;
  report: Report;
}) {
  const { run, busy } = useAction();
  const act = (operation: "stop" | "background") =>
    run(() =>
      command("activity", session, {
        operation,
        activity_id: item.id || 0,
      }).catch(report),
    );
  return (
    <>
      {item.detached === false && (
        <IconButton
          label="Move to background"
          disabled={!online || busy}
          onClick={() => act("background")}
        >
          <ArrowDownToLine />
        </IconButton>
      )}
      <IconButton
        label={`Stop ${name}`}
        disabled={!online || busy || item.status === "stopping"}
        onClick={() => act("stop")}
      >
        <Square />
      </IconButton>
    </>
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
  const name = item.label || "Command";
  const elapsed = useElapsed(active(item), item.started_ms);
  return (
    <li class="activity-row">
      <Button
        variant="quiet"
        class="activity-open"
        disabled={!online}
        onClick={open}
      >
        <Terminal />
        <span class="activity-text">
          <strong>{cleanText(name)}</strong>
          <small>
            {item.status}
            {elapsed && ` · ${elapsed}`}
            {item.progress && ` · ${cleanText(item.progress)}`}
          </small>
        </span>
      </Button>
      {active(item) && (
        <ActivityControls
          item={item}
          name={name}
          session={session}
          online={online}
          report={report}
        />
      )}
    </li>
  );
}

// An agent this conversation started: it opens as its conversation, and
// Stop interrupts its turn.
function AgentRow({
  agent,
  online,
  report,
  open,
}: {
  agent: Session;
  online: boolean;
  report: Report;
  open: () => void;
}) {
  const { run, busy } = useAction();
  const name = cleanText(agent.title || "Agent");
  return (
    <li class="activity-row">
      <Button
        variant="quiet"
        class="activity-open"
        disabled={!online}
        onClick={open}
      >
        <Users />
        <span class="activity-text">
          <strong>{name}</strong>
          <small>{cleanText(agent.turn_status || "Working")}</small>
        </span>
      </Button>
      <IconButton
        label={`Stop ${name}`}
        disabled={!online || busy}
        onClick={() => run(() => command("interrupt", agent).catch(report))}
      >
        <Square />
      </IconButton>
    </li>
  );
}
