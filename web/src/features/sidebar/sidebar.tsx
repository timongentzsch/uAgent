import {
  ConnectionStatus,
  type ConnectionPhase,
} from "../../shared/connection-status.tsx";
import { Component, type ComponentChildren } from "preact";
import type {
  Session,
  Report,
  AppModal,
  Snapshot,
} from "../../shared/types.ts";
import { useState } from "preact/hooks";
import {
  Check,
  Plus,
  RefreshCw,
  Settings,
  Library,
  CalendarClock,
  MessagesSquare,
  Search,
  CircleAlert,
  TriangleAlert,
  Inbox,
} from "lucide-preact";
import { command, manage } from "../../state/api.ts";
import {
  Mark,
  Input,
  Time,
  Button,
  IconButton,
  Placeholder,
  EmptyState,
} from "../../shared/ui.tsx";
import FolderLabel, {
  folderName,
  folderOf,
} from "../../shared/folder-label.tsx";
import SessionName from "../../shared/session-name.tsx";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { levelLabel } from "../../shared/verbosity.ts";
import { ActivityStatus, active } from "../chat/activity-status.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import {
  needsYou,
  statusOf,
  waiting,
  type SessionState,
} from "../../state/attention.ts";
import { SheetButton } from "../../shared/sheet.tsx";
import WaitingList from "../coordinator/escalations.tsx";
// The conversation's transcript as Markdown on the host; says where.
export async function shareTranscript(item: Session) {
  const result = await command("share", item);
  return result.pending ? "" : `Transcript saved to ${result.result.path}`;
}
// A fresh runtime that keeps the history, e.g. for a setting that needs it.
export async function restartConversation(item: Session) {
  const { restarting, deferred } = await manage("restart_conversations", {
    target_id: item.id,
  });
  return restarting
    ? "Conversation restarted."
    : deferred
      ? "Restarts when this turn ends."
      : "Nothing to restart: this conversation is not running.";
}

export function ConversationMenu({
  item,
  online,
  fork,
  loadSnapshot,
  report,
  notify,
  open,
  detail,
}: {
  item: Session;
  online: boolean;
  fork: (item: Session) => void;
  loadSnapshot: (id: string) => Promise<Snapshot>;
  report: Report;
  // Says what an action did, where the conversation shows its notices.
  notify: (text: string) => void;
  open: (modal: AppModal) => void;
  // How much of the agent's work the transcript shows, and changing it.
  detail?: {
    levels: string[];
    level: string;
    disabled: boolean;
    change: (level: string) => void;
  };
}) {
  return (
    <Menu label="Conversation menu">
      <MenuItem
        disabled={
          !online ||
          item.turn_active ||
          ["starting", "draft"].includes(item.status || "")
        }
        title={
          item.turn_active || ["starting", "draft"].includes(item.status || "")
            ? "Wait for the running turn to finish before forking"
            : undefined
        }
        onClick={() => fork(item)}
      >
        Fork conversation
      </MenuItem>
      <MenuItem
        disabled={!online}
        onClick={() => {
          open({ type: "rename", session: item });
        }}
      >
        Rename
      </MenuItem>
      <MenuItem
        onClick={() => open({ type: "statistics", session_id: item.id })}
      >
        Statistics
      </MenuItem>
      <MenuItem
        disabled={!online}
        onClick={() => open({ type: "tools", session_id: item.id })}
      >
        Tools
      </MenuItem>
      <MenuItem
        disabled={!online}
        onClick={() => void shareTranscript(item).then(notify, report)}
      >
        Export transcript
      </MenuItem>
      {detail?.levels.map((level) => (
        <MenuItem
          key={level}
          role="menuitemradio"
          aria-checked={level === detail.level}
          disabled={detail.disabled}
          onClick={() => detail.change(level)}
        >
          Detail: {levelLabel(level)}
          {level === detail.level && <Check />}
        </MenuItem>
      ))}
      {item.generation && (
        <MenuItem
          disabled={!online || item.turn_active}
          onClick={() =>
            void command("submit", item, { text: "/compact" }).catch(report)
          }
        >
          Compact
        </MenuItem>
      )}
      {item.generation && (
        <MenuItem
          disabled={!online}
          onClick={() => void restartConversation(item).then(notify, report)}
        >
          Restart
        </MenuItem>
      )}
      {item.generation && (
        <MenuItem
          disabled={!online}
          onClick={() =>
            command("close", item)
              .then(() => loadSnapshot(item.id))
              .catch(report)
          }
        >
          Close session
        </MenuItem>
      )}
      <MenuItem
        disabled={!online}
        onClick={() => {
          open({ type: "delete", session: item });
        }}
      >
        Delete
      </MenuItem>
    </Menu>
  );
}

// Before the catalogue arrives, the list draws these (see <Placeholder>).
const SAMPLE: Session[] = [
  "A conversation title",
  "Another title about this long",
  "A short one",
].map((title, index) => ({
  id: `placeholder-${index}`,
  title,
  cwd: "/project",
  updated: Date.now(),
}));

// A folder's coordinator: faint until used, pulsing while it works, badged
// with the decisions waiting on you, which a thread cannot proceed without.
function CoordinatorButton({
  folder,
  coordinator,
  waiting,
  online,
  open,
}: {
  folder: string;
  coordinator?: Session;
  waiting: number;
  online: boolean;
  open: () => void;
}) {
  const state = !coordinator
    ? "idle"
    : online && coordinator.turn_active
      ? "working"
      : "ready";
  // One name carries the folder, the state and the badge's count.
  const label = [
    `Coordinator for ${folderName(folder)}`,
    state === "working" && "working",
    waiting > 0 && `${waiting} waiting on you`,
  ]
    .filter(Boolean)
    .join(", ");
  return (
    <IconButton
      label={label}
      class={`coordinator-button ${state}`}
      onClick={open}
      disabled={!online}
    >
      <MessagesSquare />
      {waiting > 0 && (
        <span class="coordinator-badge" aria-hidden="true">
          {waiting}
        </span>
      )}
    </IconButton>
  );
}

// A row that waits on you or failed shows that as a shape in place of the
// LED; a working row keeps the LED, which breathes. The words beside it name
// the state.
const STATE_ICONS: Partial<Record<SessionState, typeof Inbox>> = {
  waiting: CircleAlert,
  failed: TriangleAlert,
};

// The list's folders in its order, newest first: each folder's threads
// (under its coordinator's header), then its conversations. The
// coordinator is the header, not a row.
export function groupSessions(sessions: Session[], search = "") {
  const groups = new Map<string, { threads: Session[]; others: Session[] }>();
  for (const item of [...sessions]
    .sort((a, b) => (b.updated || 0) - (a.updated || 0))
    .filter((item) =>
      `${item.title} ${item.cwd}`.toLowerCase().includes(search.toLowerCase()),
    )) {
    const folder = folderOf(item);
    // A folder with only its coordinator still has a header to open it.
    if (!groups.has(folder)) groups.set(folder, { threads: [], others: [] });
    const group = groups.get(folder)!;
    if (item.kind === "thread") group.threads.push(item);
    else if (item.kind !== "coordinator") group.others.push(item);
  }
  return groups;
}

// The rows in the order the list shows them: what Alt+↑/↓ steps through.
export const sessionOrder = (sessions: Session[]) =>
  [...groupSessions(sessions).values()].flatMap((group) => [
    ...group.threads,
    ...group.others,
  ]);

type SessionRowProps = {
  item: Session;
  selected: boolean;
  unread: boolean;
  online: boolean;
  choose: (id: string) => void;
  menu: (session: Session) => ComponentChildren;
};

// One conversation in the list: its title, activity and last update.
function SessionRowView({
  item,
  selected,
  unread,
  online,
  choose,
  menu,
}: SessionRowProps) {
  const { state } = statusOf(item, online);
  const Icon = STATE_ICONS[state];
  return (
    <div class="session-row">
      <ListRow
        class="session"
        onClick={() => choose(item.id)}
        aria-current={selected ? "page" : undefined}
        title={<SessionName item={item} />}
        unread={unread && "Unread messages"}
        meta={
          <>
            {Icon && <Icon class={`session-state ${state}`} aria-hidden />}
            <ActivityStatus
              phase={
                online &&
                (item.pending ||
                  item.turn_active ||
                  item.activities?.some(active))
                  ? item.activity || item.status
                  : ""
              }
              running={online && item.turn_active}
              present={online && !!item.presence}
              items={online ? item.activities || [] : []}
              pending={online && item.pending}
            />
            {item.updated ? <Time value={item.updated} /> : "New conversation"}
            {item.error && <span title={item.error}> · Needs attention</span>}
          </>
        }
      />
      {menu(item)}
    </div>
  );
}

const changed = (prior: object, next: object) =>
  Object.entries(next).some(
    ([key, value]) => (prior as Record<string, unknown>)[key] !== value,
  );

// A patch to one session keeps the others' objects, so only its row renders.
class SessionRow extends Component<SessionRowProps> {
  shouldComponentUpdate(next: SessionRowProps) {
    return changed(this.props, next);
  }
  render(props: SessionRowProps) {
    return <SessionRowView {...props} />;
  }
}

type SidebarProps = {
  loading: boolean;
  page: string;
  navigate: (page: "chat" | "library" | "scheduled") => void;
  scheduledUnread: boolean;
  sessions: Session[];
  selected: string;
  unread: Set<string>;
  online: boolean;
  connection: ConnectionPhase;
  choose: (id: string) => void;
  menu: (session: Session) => ComponentChildren;
  refresh: () => void;
  settings: () => void;
  create: () => void;
  coordinate: (cwd: string) => void;
  palette: () => void;
  report: Report;
};

function SidebarView({
  loading,
  sessions,
  selected,
  unread,
  online,
  connection,
  choose,
  menu,
  refresh,
  settings,
  create,
  coordinate,
  page,
  navigate,
  scheduledUnread,
  palette,
  report,
}: SidebarProps) {
  const [search, setSearch] = useState("");
  // Until the list arrives, it draws sample rows in its own layout.
  const drawing = loading && !sessions.length;
  const all = drawing ? SAMPLE : sessions;
  // Each folder's coordinator is its header icon, not a row. It and its
  // badge count every session in the folder, whatever the search shows.
  const coordinators = new Map<string, Session>();
  for (const item of all)
    if (item.kind === "coordinator") coordinators.set(folderOf(item), item);
  const row = (item: Session) => (
    <SessionRow
      key={item.id}
      item={item}
      selected={item.id === selected}
      unread={unread.has(item.id)}
      online={online}
      choose={choose}
      menu={menu}
    />
  );
  const count = drawing ? 0 : needsYou(all);
  const list = [...groupSessions(all, search)].map(([cwd, group]) => (
    <section key={cwd}>
      <h2>
        <FolderLabel path={cwd} />
        {!drawing && (
          <CoordinatorButton
            folder={cwd}
            coordinator={coordinators.get(cwd)}
            waiting={waiting(all, cwd).length}
            online={online}
            open={() => coordinate(cwd)}
          />
        )}
      </h2>
      {group.threads.length > 0 && (
        <div
          class="threads"
          role="group"
          aria-label={`Threads of the coordinator for ${folderName(cwd)}`}
        >
          {group.threads.map(row)}
        </div>
      )}
      {group.others.map(row)}
    </section>
  ));
  return (
    <>
      <div class="sidebar-head">
        <a
          class="brand"
          href="#"
          aria-label="uAgent home"
          onClick={(event) => {
            event.preventDefault();
            choose("");
          }}
        >
          <Mark />
        </a>
        <IconButton label="Command palette" onClick={palette}>
          <Search />
        </IconButton>
        <Button variant="quiet" onClick={create} disabled={!online}>
          <Plus />
          New conversation
        </Button>
      </div>
      {count > 0 && (
        <SheetButton
          label={`${count} need${count === 1 ? "s" : ""} you`}
          heading="Needs you"
          className="needs-you"
          trigger={
            <>
              <Inbox />
              {count} need{count === 1 ? "s" : ""} you
            </>
          }
        >
          {(close) => (
            <WaitingList
              sessions={all}
              online={online}
              report={report}
              choose={(id) => {
                close();
                choose(id);
              }}
            />
          )}
        </SheetButton>
      )}
      <div class="sidebar-sections">
        <Button
          variant="quiet"
          aria-current={page === "library" ? "page" : undefined}
          onClick={() => navigate("library")}
        >
          <Library />
          Library
        </Button>
        <Button
          variant="quiet"
          aria-current={page === "scheduled" ? "page" : undefined}
          onClick={() => navigate("scheduled")}
        >
          <CalendarClock />
          Scheduled
          {scheduledUnread && (
            <span
              class="unread-dot"
              role="img"
              aria-label="Unread scheduled results"
            />
          )}
        </Button>
      </div>
      <label class="search">
        <span class="sr-only">Find a conversation</span>
        <Input
          id="session-search"
          type="search"
          value={search}
          onInput={(event) => setSearch(event.currentTarget.value)}
          placeholder="Find a conversation…"
        />
      </label>
      <nav aria-label="Conversations">
        <Placeholder label="Loading conversations…" when={drawing}>
          {search && !list.length ? (
            <EmptyState>No conversation matches.</EmptyState>
          ) : (
            list
          )}
        </Placeholder>
      </nav>
      <footer>
        <ConnectionStatus
          phase={connection}
          className={`connection ${online ? "connected" : ""}`}
        />
        <IconButton label="Refresh" onClick={refresh}>
          <RefreshCw />
        </IconButton>
        <IconButton label="Settings" onClick={settings}>
          <Settings />
        </IconButton>
      </footer>
    </>
  );
}

// The shell re-renders with every streamed frame; the list only when one of
// its props changes, which the shell keeps stable.
export default class Sidebar extends Component<SidebarProps> {
  shouldComponentUpdate(next: SidebarProps) {
    return changed(this.props, next);
  }
  render(props: SidebarProps) {
    return <SidebarView {...props} />;
  }
}
