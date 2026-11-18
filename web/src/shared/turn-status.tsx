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
  ChevronDown,
  CircleAlert,
  MessagesSquare,
  Square,
  Terminal,
  Users,
  X,
} from "lucide-preact";
import {
  cleanText,
  Button,
  IconButton,
  DataText,
  updateChecked,
} from "./ui.tsx";
import { SheetButton } from "./sheet.tsx";
import { useApi } from "../state/api.ts";
import { isRunning } from "../state/attention.ts";
import { useAction } from "./use-action.ts";
import { useElapsed } from "./time.ts";
import { focusDecision, revealLiveRow } from "./navigation.ts";

// A conversation's side chats: which, and what a row of one does.
export type SideRows = {
  list: Session[];
  open: (id: string) => void;
  remove: (id: string) => void;
};

export const active = (item: Activity) =>
  ["running", "starting", "stopping", "finishing"].includes(item.status || "");

// Reads like "Working · 4m 02s · 1 command · 2 agents · 1 side chat". It
// never names a step, which its own row shows, and is absent while nothing
// needs saying. Side chats stay listed while they exist, at work or not.
function ActivityButtonView({
  items,
  agents,
  sides,
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
  agents: Session[];
  sides: SideRows;
  session: SessionRef;
  online: boolean;
  connection?: ConnectionPhase;
  running: boolean;
  started?: number;
  pending: boolean;
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
    sides.list.length ? plural(sides.list.length, "side chat") : "",
  ].filter(Boolean);
  const live =
    running ||
    rows.length + agents.length > 0 ||
    sides.list.some((side) => isRunning(side, online));
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
        size="compact"
        buttonClass="activity-button"
        sheetClass="activity-sheet"
        trigger={
          <>
            {live && <StatusLed state="running" />}
            <span class="truncate">
              <DataText>
                {running && <span class="working">Working</span>}
                {running && details && " · "}
                {details}
              </DataText>
            </span>
            <ChevronDown />
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
            {sides.list.map((side) => (
              <AgentRow
                key={side.id}
                agent={side}
                remove={() => sides.remove(side.id)}
                online={online}
                report={report}
                open={() => {
                  close();
                  sides.open(side.id);
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
  const { command } = useApi();
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

// An agent at work, or a side chat, which stays listed until it is removed.
function AgentRow({
  agent,
  remove,
  online,
  report,
  open,
}: {
  agent: Session;
  remove?: () => void;
  online: boolean;
  report: Report;
  open: () => void;
}) {
  const { command } = useApi();
  const { run, busy } = useAction();
  const name = cleanText(agent.title || (remove ? "Side chat" : "Agent"));
  const working = !remove || isRunning(agent, online);
  return (
    <li class="activity-row">
      <Button
        variant="quiet"
        class="activity-open"
        disabled={!online}
        onClick={open}
      >
        {remove ? <MessagesSquare /> : <Users />}
        <span class="activity-text">
          <strong>{name}</strong>
          <small>
            {working
              ? cleanText(agent.turn_status || "Working")
              : agent.needs_you
                ? "Needs you"
                : "Ready"}
          </small>
        </span>
      </Button>
      {working ? (
        <IconButton
          label={`Stop ${name}`}
          disabled={!online || busy}
          onClick={() => run(() => command("interrupt", agent).catch(report))}
        >
          <Square />
        </IconButton>
      ) : (
        <IconButton
          label={`Remove ${name} from the side chats`}
          onClick={remove}
        >
          <X />
        </IconButton>
      )}
    </li>
  );
}
