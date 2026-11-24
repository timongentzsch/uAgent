import "./activity.css";
import { ConnectionStatus, StatusLed } from "./connection-status.tsx";
import { WorkBand } from "./work-band.tsx";
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
} from "lucide-preact";
import {
  cleanText,
  Button,
  IconButton,
  DataText,
  Time,
  updateChecked,
} from "./ui.tsx";
import { SheetButton } from "./sheet.tsx";
import { useApi } from "../state/api.ts";
import { AgentAssignment } from "./agent-assignment.tsx";
import { shortAgentName } from "./display.ts";
import { isRunning, statusOf } from "../state/attention.ts";
import { useAction } from "./use-action.ts";
import { useElapsed } from "./time.ts";
import { focusDecision, revealLiveRow } from "./navigation.ts";

// A conversation's side chats: which, those whose first question is still
// on its way (by its request), and what a row of one does.
export type SideRows = {
  list: Session[];
  starting: string[];
  open: (id: string) => void;
};

export const active = (item: Activity) =>
  ["running", "starting", "stopping", "finishing"].includes(item.status || "");

// Reads like "Working · 4m 02s · 1 command · 1 side chat". It
// never names a step, which its own row shows, and is absent while nothing
// needs saying. Side chats stay listed while they exist, at work or not.
function ActivityButtonView({
  items,
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
}: {
  items: Activity[];
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
}) {
  // A listener hears a turn's edges: when a response starts, needs input
  // or ends.
  const responded = useRef(false);
  if (running) responded.current = true;
  const elapsed = useElapsed(running && !pending, started);
  const rows = items.filter(active);
  const counts = [
    rows.length ? plural(rows.length, "command") : "",
    sides.list.length ? plural(sides.list.length, "side chat") : "",
  ].filter(Boolean);
  const live =
    running ||
    rows.length > 0 ||
    sides.starting.length > 0 ||
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
            {live && <WorkBand />}
            <span class="truncate">
              <DataText>
                {running && "Working"}
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
            {sideOrder(sides.list, online).map((side) => (
              <AgentRow
                key={side.id}
                agent={side}
                starting={sides.starting.includes(side.side!.request)}
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

type AgentEntry = {
  agent: Session;
  title: string;
  name: string;
  status: string;
};
type AgentListProps = {
  entries: AgentEntry[];
  online: boolean;
  report: Report;
  choose: (id: string) => void;
};

// The host distinguishes a request routed to the parent from one that
// actually needs the person. Both conversations are still active.
function agentRosterStatus(agent: Session) {
  if (agent.needs_you) return "Needs you";
  if (agent.status === "waiting") return "Waiting";
  if (agent.status === "starting") return "Starting";
  if (agent.turn_active || agent.status === "running") return "Working";
  if (agent.status === "interrupted") return "Interrupted";
  if (agent.error || agent.status === "failed") return "Failed";
  return "Finished";
}

export function groupAgents(
  agents: Session[],
  names: ReadonlyMap<string, string>,
  sides: Session[],
) {
  const current: AgentEntry[] = [];
  const past: AgentEntry[] = [];
  const sideIds = new Set(sides.map((side) => side.id));
  const byName = new Map<string, AgentEntry[]>();
  for (const agent of agents) {
    if (sideIds.has(agent.id)) continue;
    const title = names.get(agent.id) || agent.title || "Agent";
    const name = shortAgentName(title);
    const status = agentRosterStatus(agent);
    const entry = { agent, title, name, status };
    (["Needs you", "Waiting", "Starting", "Working"].includes(status)
      ? current
      : past
    ).push(entry);
    const matches = byName.get(name);
    if (matches) matches.push(entry);
    else byName.set(name, [entry]);
  }
  // A shared opening sentence is common in older assignments. Give each
  // conversation a stable, readable number even when catalogue order changes.
  for (const matches of byName.values()) {
    if (matches.length < 2) continue;
    matches.sort((a, b) => a.agent.id.localeCompare(b.agent.id));
    matches.forEach((entry, i) => {
      entry.name =
        (entry.name.endsWith("…") ? "Agent" : entry.name) + " " + (i + 1);
    });
  }
  return { current, past };
}

export const AgentRoster = updateChecked(function AgentRoster({
  entries,
  online,
  report,
  choose,
  past = false,
}: AgentListProps & { past?: boolean }) {
  if (!entries.length) return null;
  const working =
    !past &&
    online &&
    entries.some(({ status }) => status === "Starting" || status === "Working");
  const label = past
    ? `Past agents (${entries.length})`
    : plural(entries.length, "active agent");
  return (
    <SheetButton
      label={label}
      heading={past ? "Past agents" : "Active agents"}
      size="compact"
      buttonClass="agent-roster small"
      trigger={
        <>
          {working ? <WorkBand /> : <Users aria-hidden="true" />}
          {past ? label : entries.length}
        </>
      }
    >
      {(close) => (
        <ul class="activity-list divided">
          {entries.map(({ agent, title, name, status }) => (
            <AgentRow
              key={agent.id}
              agent={agent}
              title={title}
              name={name}
              status={status}
              report={report}
              online={online}
              open={() => {
                close();
                choose(agent.id);
              }}
            />
          ))}
        </ul>
      )}
    </SheetButton>
  );
});

export const AgentBar = updateChecked(function AgentBar(props: AgentListProps) {
  if (!props.entries.length) return null;
  return (
    <nav class="agent-bar" aria-label="Active agents">
      <AgentRoster {...props} />
      <div class="agent-tabs">
        {props.entries.map(({ agent, title, name, status }) => (
          <Button
            key={agent.id}
            variant="quiet"
            class="agent-tab small"
            title={title}
            aria-label={name + " · " + status}
            onClick={() => props.choose(agent.id)}
          >
            {agent.needs_you && <CircleAlert aria-hidden="true" />}
            <span class="truncate">{name}</span>
            <span class="muted">{status}</span>
          </Button>
        ))}
      </div>
    </nav>
  );
});

// Side chats at work or waiting for you first, then the newest asked.
const sideOrder = (list: Session[], online: boolean) =>
  [...list].sort(
    (a, b) =>
      Number(statusOf(b, online) !== "idle") -
        Number(statusOf(a, online) !== "idle") || b.side!.asked - a.side!.asked,
  );

// The step a turn is at, where it says more than that it works.
const stepOf = (item: Session) =>
  item.turn_status && !["Working", "Ready"].includes(item.turn_status)
    ? cleanText(item.turn_status)
    : "";

// Agent rosters and the side chat activity list share a row.
function AgentRow({
  agent,
  title,
  name: label,
  status,
  starting = false,
  online,
  report,
  open,
}: {
  agent: Session;
  title?: string;
  name?: string;
  status?: string;
  starting?: boolean;
  online: boolean;
  report: Report;
  open: () => void;
}) {
  const { command } = useApi();
  const { run, busy } = useAction();
  const side = agent.side;
  const name = cleanText(
    label || agent.title || (side ? "Side chat" : "Agent"),
  );
  const running = isRunning(agent, online);
  return (
    <li class={side ? "activity-row" : "activity-row agent-entry"}>
      <Button
        variant="quiet"
        class="activity-open"
        disabled={!online}
        onClick={open}
      >
        {side && <MessagesSquare />}
        <span class="activity-text">
          <strong>{name}</strong>
          <small>
            {status ||
              (online && agent.needs_you
                ? "Needs you"
                : running || starting
                  ? ["Working", stepOf(agent)].filter(Boolean).join(" · ")
                  : "Ready")}
            {side?.merged && (
              <>
                {" · merged "}
                <Time value={side.merged.at} />
              </>
            )}
          </small>
        </span>
      </Button>
      {running && (
        <IconButton
          label={`Stop ${name}`}
          disabled={!online || busy}
          onClick={() => run(() => command("interrupt", agent).catch(report))}
        >
          <Square />
        </IconButton>
      )}
      {!side && (
        <AgentAssignment
          title={title || name}
          name={name}
          edits={agent.agent_edits}
        />
      )}
    </li>
  );
}
