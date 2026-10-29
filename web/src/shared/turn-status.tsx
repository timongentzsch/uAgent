import "./activity.css";
import { ConnectionStatus, StatusLed } from "./connection-status.tsx";
import { plural } from "./quantities.ts";
import type {
  Activity,
  ConnectionPhase,
  InspectorTarget,
  Pending,
  Report,
  SessionRef,
} from "./types.ts";
import { useRef } from "preact/hooks";
import {
  ArrowDownToLine,
  ChevronUp,
  Layers,
  Square,
  Terminal,
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
// The turn's status line above the composer and the controls for the
// background commands the turn started.

export const active = (item: Activity) =>
  ["running", "starting", "stopping", "finishing"].includes(item.status || "");
export function TurnStatus({
  phase = "Ready",
  running,
  started,
  pending,
  stopped,
  announce = false,
  present = false,
  connection,
}: {
  phase?: string;
  running?: boolean;
  // When the running turn started: its time so far follows the caption.
  started?: number;
  pending?: Pending | boolean | null;
  // Why the last turn stopped short ("Stopped"): shown and announced in
  // place of the phase until something new is sent.
  stopped?: string;
  announce?: boolean;
  present?: boolean;
  connection?: ConnectionPhase;
}) {
  // A listener hears a turn's edges, not every step between them: the
  // visible phase changes with each tool call, the announcement only when
  // a response starts, needs input or ends.
  const responded = useRef(false);
  if (running) responded.current = true;
  const detail = useElapsed(!!running, started);
  if (connection && connection !== "connected")
    return <ConnectionStatus phase={connection} />;
  const announcement = pending
    ? "Needs your input"
    : running
      ? "Responding"
      : stopped
        ? stopped
        : responded.current
          ? "Response complete"
          : "";
  const caption = pending
    ? "Needs your input"
    : !running && stopped
      ? stopped
      : phase;
  return (
    <span class="turn-status">
      <StatusLed
        state={running && !pending ? "running" : present ? "active" : "idle"}
      />
      <span class="turn-status-caption" title={caption}>
        {caption}
      </span>
      {detail && <span class="turn-status-counts"> · {detail}</span>}
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

// Always under the input: what is running. Finished commands live only in
// the conversation.
function ActivityButtonView({
  items,
  session,
  online,
  report,
  open,
}: {
  items: Activity[];
  session: SessionRef;
  online: boolean;
  report: Report;
  open: (target: InspectorTarget) => void;
}) {
  const rows = items.filter(active);
  const counts = rows.length ? plural(rows.length, "command") : "";
  return (
    <SheetButton
      label="Activity"
      buttonClass={`activity-button${counts ? "" : " idle"}`}
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
                <DataText>No activity</DataText>
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
          <ul class="activity-list divided">
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
          </ul>
        ) : (
          <p class="muted small">
            Nothing running. Background commands appear here.
          </p>
        )
      }
    </SheetButton>
  );
}
// The composer renders with every key pressed; the button only when the
// work it lists changes.
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
