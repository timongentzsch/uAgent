import { HostLabel } from "../../shared/host-scope.tsx";
import { useState } from "preact/hooks";
import { CircleAlert, LayoutGrid, List, SquareTerminal } from "lucide-preact";
import type {
  FolderFacts,
  Session,
  Space,
  TerminalInfo,
} from "../../shared/types.ts";
import { useApi } from "../../state/api.ts";
import {
  childrenByParent,
  isAgent,
  statusOf,
  type SessionState,
} from "../../state/attention.ts";
import { useHostContext } from "../../state/host-context.ts";
import { folderOf } from "../../shared/display.ts";
import { folderName } from "../../shared/folder-label.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { cost, plural } from "../../shared/quantities.ts";
import { useNarrowColumn } from "../../shared/layout.ts";
import { useResource } from "../../shared/use-resource.ts";
import { stored } from "../../shared/storage.ts";
import { storageKeys } from "../../shared/storage-keys.ts";
import WaitingList from "../../shared/escalations.tsx";
import { StartField } from "../../shared/start-field.tsx";
import SessionName from "../../shared/session-name.tsx";
import {
  Bar,
  Button,
  DataText,
  EmptyState,
  IconButton,
  Notice,
  Row,
  SectionTitle,
  Time,
} from "../../shared/ui.tsx";
import "./overview.css";

const WORDS: Record<SessionState, string> = {
  working: "Working",
  waiting: "Needs you",
  failed: "Stopped",
  idle: "Idle",
};
const DAY = 24 * 60 * 60 * 1000;
// Chosen on this device; unset follows the screen.
const layoutKept = stored<"" | "grid" | "list">(storageKeys.overview, "");

export default function FolderOverview({
  folder,
  space,
  sessions,
  choose,
  start,
  terminals,
  showTerminal,
  newTerminal,
}: {
  folder: string;
  space?: Space;
  sessions: Session[];
  choose: (id: string) => void;
  start: (folder: string, text: string) => Promise<unknown>;
  terminals?: TerminalInfo[] | null;
  showTerminal: (terminal: TerminalInfo) => void;
  newTerminal?: (folder: string) => void;
}) {
  const { api, routes } = useApi();
  const { online } = useHostContext();
  const narrow = useNarrowColumn();
  const [kept, keep] = useState(layoutKept.read);
  const layout = kept || (narrow ? "list" : "grid");
  const name = space?.name || folderName(folder);
  const here = sessions.filter((item) => folderOf(item) === folder);
  const roots = here
    .filter((item) => !item.parent)
    .sort((a, b) => (b.updated || 0) - (a.updated || 0));
  const agentsHere = here.filter(isAgent);
  const children = childrenByParent(agentsHere);
  const state = (item: Session) => statusOf(item, online);
  // Read again whenever one of its conversations changes.
  const changed = Math.max(0, ...here.map((item) => item.updated || 0));
  const stats = useResource(
    async (signal) => ({
      folder,
      facts: await api<FolderFacts>(routes.folder(folder), undefined, {
        signal,
      }),
    }),
    [folder, changed, api],
  );
  const facts = stats.value?.folder === folder ? stats.value.facts : undefined;
  const missing = stats.error ? "Unavailable" : "Loading…";
  const working = roots.filter((item) => state(item) === "working").length;
  const agentsWorking = agentsHere.filter(
    (item) => state(item) === "working",
  ).length;
  const waiting = here.filter((item) => state(item) === "waiting").length;
  const week = roots.filter(
    (item) => Date.now() - (item.updated || 0) < 7 * DAY,
  );
  const git = facts?.git;
  const shells = (terminals || []).filter((item) => item.folder === folder);
  const choice = (value: "grid" | "list") => {
    keep(value);
    layoutKept.write(value);
  };
  return (
    <section class="overview" aria-label={`Overview of ${name}`}>
      {/* raw: the overview's heading, a block and not a bar. */}
      <header class="overview-head">
        <HostLabel />
        <h2>
          <DataText>{name}</DataText>
        </h2>
        <p class="small">
          <DataText>
            {[
              folder,
              git?.branch,
              git && plural(git.uncommitted, "file") + " uncommitted",
            ]
              .filter(Boolean)
              .join(" · ")}
          </DataText>
        </p>
      </header>
      <StartField
        label={`Start a conversation in ${name}`}
        placeholder={`Start a conversation in ${name}…`}
        disabled={!online}
        start={(text) => start(folder, text)}
      >
        {() =>
          newTerminal && (
            <Button disabled={!online} onClick={() => newTerminal(folder)}>
              New terminal
            </Button>
          )
        }
      </StartField>
      {waiting > 0 && (
        <div class="overview-section">
          <SectionTitle>Needs you</SectionTitle>
          <WaitingList sessions={here} choose={choose} />
        </div>
      )}
      {stats.error != null && (
        <Notice
          tone="warning"
          action={<Button onClick={stats.retry}>Retry</Button>}
        >
          {facts
            ? "Couldn’t refresh folder statistics. Showing the last values."
            : "Couldn’t load folder statistics."}
        </Notice>
      )}
      <dl class="overview-stats card divided" aria-label="Summary">
        <Stat
          label="Working"
          value={working}
          detail={`conversations · ${plural(agentsWorking, "agent")}`}
        />
        <Stat label="Needs you" value={waiting} detail="decisions" />
        <Stat
          label="Today"
          value={facts ? cost(facts.spend.today) : missing}
          detail={facts ? `7 days ${cost(facts.spend.week)}` : ""}
        />
        <Stat
          label="Changed today"
          value={facts ? plural(facts.changed.files, "file") : missing}
          detail={
            facts ? `+${facts.changed.added} −${facts.changed.removed}` : ""
          }
        />
        <Stat
          label="Conversations"
          value={roots.length}
          detail={`${week.length} this week`}
        />
      </dl>
      {shells.length > 0 && (
        <div class="overview-section">
          <SectionTitle>Terminals</SectionTitle>
          <div class="divided">
            {shells.map((terminal) => (
              <Row
                key={terminal.id}
                icon={<SquareTerminal />}
                label={terminal.title}
                detail={terminal.cwd}
                onClick={() => showTerminal(terminal)}
              />
            ))}
          </div>
        </div>
      )}
      <div class="overview-section">
        <Bar density="flush" class="overview-caption">
          <SectionTitle>Conversations</SectionTitle>
          <div class="segmented" role="group" aria-label="Layout">
            <IconButton
              label="Show as cards"
              aria-pressed={layout === "grid"}
              onClick={() => choice("grid")}
            >
              <LayoutGrid />
            </IconButton>
            <IconButton
              label="Show as a list"
              aria-pressed={layout === "list"}
              onClick={() => choice("list")}
            >
              <List />
            </IconButton>
          </div>
        </Bar>
        {roots.length ? (
          <ul
            class={`overview-${layout}${layout === "list" ? " divided" : ""}`}
          >
            {roots.map((item) => {
              const agents = children.get(item.id) || [];
              return (
                <li key={item.id}>
                  <Card
                    item={item}
                    state={state(item)}
                    agents={agents.length}
                    working={
                      agents.filter((a) => state(a) === "working").length
                    }
                    card={layout === "grid"}
                    open={() => choose(item.id)}
                  />
                </li>
              );
            })}
          </ul>
        ) : (
          <EmptyState>No conversations here yet.</EmptyState>
        )}
      </div>
    </section>
  );
}

function Stat({
  label,
  value,
  detail,
}: {
  label: string;
  value: string | number;
  detail: string;
}) {
  return (
    <div class="overview-stat">
      <dt class="small">{label}</dt>
      <dd>
        <strong>{value}</strong>
        <small>{detail}</small>
      </dd>
    </div>
  );
}

function Card({
  item,
  state,
  agents,
  working,
  card,
  open,
}: {
  item: Session;
  state: SessionState;
  agents: number;
  working: number;
  card: boolean;
  open: () => void;
}) {
  const step =
    state === "waiting"
      ? item.pending_prompt
      : state === "working"
        ? item.turn_status
        : item.error;
  const todo = item.todo;
  return (
    <Button
      variant="quiet"
      class={`overview-card${card ? " card" : ""}`}
      data-state={state}
      onClick={open}
    >
      <span class="overview-title">
        <span class="truncate">
          <SessionName item={item} />
        </span>
        <span class="overview-state small">
          {state === "waiting" ? (
            <CircleAlert />
          ) : (
            <StatusLed
              state={
                state === "working"
                  ? "running"
                  : state === "failed"
                    ? "failed"
                    : "idle"
              }
            />
          )}
          {WORDS[state]}
        </span>
      </span>
      {step && (
        <small class="overview-step truncate">
          <DataText>{step}</DataText>
        </small>
      )}
      {todo && todo.done < todo.total && (
        <span class="overview-todo small">
          <progress max={todo.total} value={todo.done} />
          <DataText>
            {`${todo.done}/${todo.total}${todo.current ? ` · ${todo.current}` : ""}`}
          </DataText>
        </span>
      )}
      <small class="overview-foot">
        <span>
          {agents
            ? `${plural(agents, "agent")}${working ? ` · ${working} working` : ""}`
            : "No agents"}
        </span>
        <span>
          {item.updated && <Time value={item.updated} />} ·{" "}
          {cost(item.cost || 0)}
        </span>
      </small>
    </Button>
  );
}
