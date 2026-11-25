import "./activity.css";
import { ConnectionStatus, StatusLed } from "./connection-status.tsx";
import { WorkBand } from "./work-band.tsx";
import { count, plural } from "./quantities.ts";
import type {
  Activity,
  ConnectionPhase,
  InspectorTarget,
  Report,
  Session,
  SessionRef,
} from "./types.ts";
import type { ComponentChildren } from "preact";
import { useRef } from "preact/hooks";
import {
  ArrowDownToLine,
  Bot,
  ChevronDown,
  CircleAlert,
  MessagesSquare,
  Square,
  Terminal,
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
import { agentScope } from "./display.ts";
import {
  agentGroups,
  atWork,
  isRunning,
  statusOf,
  statusWord,
} from "../state/attention.ts";
import { useAction } from "./use-action.ts";
import { useElapsed } from "./time.ts";
import { focusDecision, revealLiveRow } from "./navigation.ts";

// A conversation's side chats: which, those whose first question is still
// on its way (by its request), what a row of one does, and its menu.
export type SideRows = {
  list: Session[];
  starting: string[];
  open: (id: string) => void;
  menu: (side: Session) => ComponentChildren;
};

export const active = (item: Activity) =>
  ["running", "starting", "stopping", "finishing"].includes(item.status || "");

// Reads like "Working · 4m 02s · 1 command · 1 side chat". It
// never names a step, which its own row shows, and is absent while nothing
// needs saying. Side chats stay listed while they exist, at work or not.
function ActivityButtonView({
  items,
  sides,
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
}: {
  items: Activity[];
  sides: SideRows;
  agents: Session[];
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
  const working = agents.filter(atWork);
  const waitingFor =
    !running && working.length
      ? "Waiting for " +
        (working.length > 2
          ? plural(working.length, "agent")
          : working.map(agentName).join(", "))
      : "";
  if (waitingFor && !rows.length && !sides.list.length)
    return (
      <>
        <span class="activity-state">
          <DataText>{waitingFor}</DataText>
        </span>
        {announce}
      </>
    );
  const details = [waitingFor, elapsed, ...counts].filter(Boolean).join(" · ");
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
                menu={sides.menu(side)}
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

const STACK = 4;

// The agents at work, a line each above the input; the rest in the panel.
export const AgentStack = updateChecked(function AgentStack(
  props: AgentsProps,
) {
  const working = props.agents.filter(atWork);
  if (!working.length) return null;
  return (
    <nav class="agent-stack" aria-label="Agents at work">
      <ul class="agent-rows activity-list">
        {working.slice(0, STACK).map((agent) => (
          <AgentRow
            key={agent.id}
            agent={agent}
            view="stack"
            online={props.online}
            report={props.report}
            open={() => props.choose(agent.id)}
          />
        ))}
      </ul>
      {working.length > STACK && (
        <AgentsPanel
          {...props}
          label={`+${working.length - STACK} more`}
          buttonClass="agent-more small"
        >
          +{working.length - STACK} more
        </AgentsPanel>
      )}
    </nav>
  );
});

type AgentsProps = {
  agents: Session[];
  // What each finished agent last reported, by session: its first line.
  reports?: Record<string, string>;
  online: boolean;
  report: Report;
  choose: (id: string) => void;
};

// Every agent of the conversation, running or finished, by the call that
// started it. `children` is what the button shows.
function AgentsPanel({
  agents,
  reports,
  online,
  report,
  choose,
  label,
  buttonClass,
  children,
}: AgentsProps & {
  label: string;
  buttonClass: string;
  children: ComponentChildren;
}) {
  return (
    <SheetButton
      label={label}
      heading="Agents"
      size="compact"
      buttonClass={buttonClass}
      trigger={children}
    >
      {(close) =>
        agentGroups(agents).map(({ agents: members, at }) => {
          const task = cleanText(members[0].title || "");
          const shown =
            members.every((a) => cleanText(a.title || "") === task) &&
            (members.length > 1 || task !== agentName(members[0]));
          return (
            <section key={members[0].id} class="agent-group">
              <h3 class="section-title caption">
                <DataText>
                  <Time value={at} /> · {plural(members.length, "agent")} ·{" "}
                  {plural(sum(members, "tokens"), "token")}
                </DataText>
              </h3>
              {shown && task && (
                <p class="agent-task small muted truncate">{task}</p>
              )}
              <ul class="activity-list">
                {members.map((agent) => (
                  <AgentRow
                    key={agent.id}
                    agent={agent}
                    view="entry"
                    line={reports?.[agent.id]}
                    online={online}
                    report={report}
                    open={() => {
                      close();
                      choose(agent.id);
                    }}
                  />
                ))}
              </ul>
            </section>
          );
        })
      }
    </SheetButton>
  );
}

const sum = (list: Session[], key: "tokens") =>
  list.reduce((total, item) => total + (item[key] || 0), 0);

// The one button in the status line: how many agents, and that some work.
export const AgentsButton = updateChecked(function AgentsButton(
  props: AgentsProps,
) {
  if (!props.agents.length) return null;
  const working =
    props.online &&
    props.agents.some((a) => ["Starting", "Working"].includes(statusWord(a)));
  return (
    <AgentsPanel
      {...props}
      label={`Agents ${props.agents.length}`}
      buttonClass="agents-button small"
    >
      {working ? <WorkBand /> : <Bot aria-hidden="true" />}
      Agents {props.agents.length}
    </AgentsPanel>
  );
});

export const agentName = (agent: Session) =>
  cleanText(agent.agent_name || agent.title || "Agent");

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
    ? cleanText(item.turn_status).replace(/^(?:Running|Preparing) · /, "")
    : "";

// An agent's row (in the stack or the panel) and a side chat's in the
// Activity list share one component.
export function AgentRow({
  agent,
  view,
  line,
  starting = false,
  menu,
  online,
  report,
  open,
}: {
  agent: Session;
  view?: "stack" | "entry";
  // The first line of what a finished agent reported.
  line?: string;
  starting?: boolean;
  menu?: ComponentChildren;
  online: boolean;
  report: Report;
  open: () => void;
}) {
  const { command } = useApi();
  const { run, busy } = useAction();
  const side = agent.side;
  const name = side ? cleanText(agent.title || "Side chat") : agentName(agent);
  const running = isRunning(agent, online);
  const word = statusWord(agent);
  const elapsed = useElapsed(!side && atWork(agent), agent.turn_started_ms);
  const calls = agent.tool_calls ? plural(agent.tool_calls, "call") : "";
  const tokens = agent.tokens ? plural(agent.tokens, "token") : "";
  const step = word === "Working" ? stepOf(agent) : "";
  const scope = agent.agent_edits !== "none" && agentScope(agent.agent_edits);
  const stop = running && (
    <IconButton
      label={`Stop ${name}`}
      disabled={!online || busy}
      onClick={() => run(() => command("interrupt", agent).catch(report))}
    >
      <Square />
    </IconButton>
  );
  if (view === "stack")
    return (
      <li class="activity-row">
        <Button
          variant="quiet"
          class="activity-open"
          disabled={!online}
          onClick={open}
        >
          <Bot />
          <strong class="truncate">{name}</strong>
          <span class="agent-step muted">
            {agent.needs_you && <CircleAlert aria-hidden="true" />}
            <span class="truncate">{step || word}</span>
          </span>
          <span class="agent-facts small muted">
            {calls && <span>{calls}</span>}
            {agent.tokens ? (
              <span class="agent-tokens">{count(agent.tokens)}</span>
            ) : null}
            {elapsed && <span class="agent-time">{elapsed}</span>}
          </span>
        </Button>
        {stop}
      </li>
    );
  if (view === "entry")
    return (
      <li class="activity-row">
        <Button
          variant="quiet"
          class="activity-open"
          disabled={!online}
          onClick={open}
        >
          <Bot />
          <span class="activity-text">
            <strong>{name}</strong>
            <small class="agent-figures">
              {[calls, tokens, elapsed, scope].filter(Boolean).join(" · ")}
            </small>
            {(step || !atWork(agent)) && (step || line) && (
              <small>{step || line}</small>
            )}
          </span>
          <span class="agent-status small muted">{word}</span>
        </Button>
        {stop}
      </li>
    );
  return (
    <li class="activity-row">
      <Button
        variant="quiet"
        class="activity-open"
        disabled={!online}
        onClick={open}
      >
        <MessagesSquare />
        <span class="activity-text">
          <strong>{name}</strong>
          <small>
            {online && agent.needs_you
              ? "Needs you"
              : running || starting
                ? ["Working", stepOf(agent)].filter(Boolean).join(" · ")
                : "Ready"}
            {side?.merged && (
              <>
                {" · merged "}
                <Time value={side.merged.at} />
              </>
            )}
          </small>
        </span>
      </Button>
      {stop}
      {menu}
    </li>
  );
}
