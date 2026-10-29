import type { TargetedSubmitEvent } from "preact";
import { useState } from "preact/hooks";
import { CircleAlert, LayoutGrid, List, SquareTerminal } from "lucide-preact";
import type {
  FolderFacts,
  Session,
  Space,
  TerminalInfo,
} from "../../shared/types.ts";
import { api, routes } from "../../state/api.ts";
import { statusOf, type SessionState } from "../../state/attention.ts";
import { useHostContext } from "../../state/host-context.ts";
import { folderOf } from "../../shared/display.ts";
import { folderName } from "../../shared/folder-label.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { cost, plural } from "../../shared/quantities.ts";
import { usePhone } from "../../shared/layout.ts";
import { useResource } from "../../shared/use-resource.ts";
import { stored } from "../../shared/storage.ts";
import { storageKeys } from "../../shared/storage-keys.ts";
import WaitingList from "../../shared/escalations.tsx";
import SessionName from "../../shared/session-name.tsx";
import {
  Button,
  DataText,
  EmptyState,
  IconButton,
  Input,
  Row,
  SectionTitle,
  Time,
} from "../../shared/ui.tsx";
import "./overview.css";

// A folder or workspace at a glance, in the conversation's place: where it
// stands, a field that starts a conversation in it, what it spent and
// changed, what waits on you, and its conversations as cards or a list.

const WORDS: Record<SessionState, string> = {
  working: "Working",
  waiting: "Needs you",
  failed: "Stopped",
  idle: "Idle",
};
const DAY = 24 * 60 * 60 * 1000;
// Grid or list, chosen on this device; unset follows the screen.
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
  // A new conversation in the folder, with its first message.
  start: (folder: string, text: string) => Promise<unknown>;
  // The folders' terminals, and opening one; a new one while the host
  // offers them.
  terminals?: TerminalInfo[] | null;
  showTerminal: (terminal: TerminalInfo) => void;
  newTerminal?: (folder: string) => void;
}) {
  const { online, report } = useHostContext();
  const phone = usePhone();
  const [kept, keep] = useState(layoutKept.read);
  const layout = kept || (phone ? "list" : "grid");
  const [text, setText] = useState("");
  const name = space?.name || folderName(folder);
  const here = sessions.filter((item) => folderOf(item) === folder);
  const roots = here
    .filter((item) => !item.parent)
    .sort((a, b) => (b.updated || 0) - (a.updated || 0));
  const agents = (item: Session) => here.filter((a) => a.parent === item.id);
  const state = (item: Session) => statusOf(item, online);
  // Read again whenever one of its conversations changes.
  const changed = Math.max(0, ...here.map((item) => item.updated || 0));
  const { value: facts } = useResource(
    () => api<FolderFacts>(routes.folder(folder)),
    [folder, changed],
  );
  const working = roots.filter((item) => state(item) === "working").length;
  const agentsWorking = here.filter(
    (item) => item.parent && state(item) === "working",
  ).length;
  const waiting = here.filter((item) => state(item) === "waiting").length;
  const week = roots.filter(
    (item) => Date.now() - (item.updated || 0) < 7 * DAY,
  );
  const git = facts?.git;
  const shells = (terminals || []).filter((item) => item.folder === folder);
  const submit = (event: TargetedSubmitEvent<HTMLFormElement>) => {
    event.preventDefault();
    start(folder, text.trim()).then(() => setText(""), report);
  };
  const choice = (value: "grid" | "list") => {
    keep(value);
    layoutKept.write(value);
  };
  return (
    <section class="overview" aria-label={`Overview of ${name}`}>
      <header class="overview-head">
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
      <form class="overview-start" onSubmit={submit}>
        <Input
          aria-label={`Start a conversation in ${name}`}
          placeholder={`Start a conversation in ${name}…`}
          value={text}
          disabled={!online}
          onInput={(event) => setText(event.currentTarget.value)}
        />
        <Button variant="primary" type="submit" disabled={!online}>
          Start
        </Button>
        {newTerminal && (
          <Button disabled={!online} onClick={() => newTerminal(folder)}>
            New terminal
          </Button>
        )}
      </form>
      <dl class="overview-stats card divided" aria-label="Summary">
        <Stat
          label="Working"
          value={working}
          detail={`conversations · ${plural(agentsWorking, "agent")}`}
        />
        <Stat label="Needs you" value={waiting} detail="decisions" />
        <Stat
          label="Today"
          value={cost(facts?.spend.today || 0)}
          detail={`7 days ${cost(facts?.spend.week || 0)}`}
        />
        <Stat
          label="Changed today"
          value={plural(facts?.changed.files || 0, "file")}
          detail={`+${facts?.changed.added || 0} −${facts?.changed.removed || 0}`}
        />
        <Stat
          label="Conversations"
          value={roots.length}
          detail={`${week.length} this week`}
        />
      </dl>
      {waiting > 0 && (
        <div class="overview-section">
          <SectionTitle>Needs you</SectionTitle>
          <WaitingList sessions={here} folder={folder} choose={choose} />
        </div>
      )}
      {shells.length > 0 && (
        <div class="overview-section">
          <SectionTitle>Terminals</SectionTitle>
          <div class="divided">
            {shells.map((terminal, index) => (
              <Row
                key={terminal.id}
                icon={<SquareTerminal />}
                label={`Terminal${shells.length > 1 ? ` ${index + 1}` : ""}`}
                detail={terminal.cwd}
                onClick={() => showTerminal(terminal)}
              />
            ))}
          </div>
        </div>
      )}
      <div class="overview-section">
        <div class="overview-caption">
          <SectionTitle>Conversations</SectionTitle>
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
        {roots.length ? (
          <ul
            class={`overview-${layout}${layout === "list" ? " divided" : ""}`}
          >
            {roots.map((item) => (
              <li key={item.id}>
                <Card
                  item={item}
                  state={state(item)}
                  agents={agents(item)}
                  working={
                    agents(item).filter((a) => state(a) === "working").length
                  }
                  card={layout === "grid"}
                  open={() => choose(item.id)}
                />
              </li>
            ))}
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

// One conversation: its state, the step it is on (the host's label), how
// far its to-do list is, its agents, when it last changed and its cost.
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
  agents: Session[];
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
        <span>
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
        <small class="overview-step">
          <DataText>{step}</DataText>
        </small>
      )}
      {todo && (
        <span class="overview-todo small">
          <progress max={todo.total} value={todo.done} />
          <DataText>
            {`${todo.done}/${todo.total}${todo.current ? ` · ${todo.current}` : ""}`}
          </DataText>
        </span>
      )}
      <small class="overview-foot">
        <span>
          {agents.length
            ? `${plural(agents.length, "agent")}${working ? ` · ${working} working` : ""}`
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
