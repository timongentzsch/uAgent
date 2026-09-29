import {
  ConnectionStatus,
  type ConnectionPhase,
} from "../../shared/connection-status.tsx";
import type { ComponentChildren } from "preact";
import type {
  Session,
  Report,
  AppModal,
  Snapshot,
} from "../../shared/types.ts";
import { useState } from "preact/hooks";
import {
  Plus,
  RefreshCw,
  Settings,
  Library,
  CalendarClock,
  MessagesSquare,
} from "lucide-preact";
import { command } from "../../state/api.ts";
import {
  Mark,
  Input,
  Time,
  Button,
  IconButton,
  Placeholder,
} from "../../shared/ui.tsx";
import FolderLabel, { folderName, folderOf } from "./folder-label.tsx";
import SessionName from "./session-name.tsx";
import { Menu, MenuItem } from "../../shared/popover.tsx";
import { ActivityStatus, active } from "../chat/activity-status.tsx";
export function ConversationMenu({
  item,
  online,
  refresh,
  choose,
  loadSnapshot,
  report,
  open,
}: {
  item: Session;
  online: boolean;
  refresh: () => Promise<void>;
  choose: (id: string) => Promise<void>;
  loadSnapshot: (id: string) => Promise<Snapshot>;
  report: Report;
  open: (modal: AppModal) => void;
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
        onClick={async () => {
          try {
            const response = await command("fork", item);
            if (response.pending) return;
            await refresh();
            await choose(response.result.id);
            const fork = { id: response.result.id, generation: "" };
            await command("activate", fork);
            await loadSnapshot(fork.id);
          } catch (error) {
            report(error);
          }
        }}
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
      <MessagesSquare aria-hidden="true" />
      {waiting > 0 && (
        <span class="coordinator-badge" aria-hidden="true">
          {waiting}
        </span>
      )}
    </IconButton>
  );
}

// One conversation in the list: its title, activity and last update.
function SessionRow({
  item,
  selected,
  unread,
  online,
  choose,
  menu,
}: {
  item: Session;
  selected: boolean;
  unread: boolean;
  online: boolean;
  choose: (id: string) => void;
  menu: (session: Session) => ComponentChildren;
}) {
  return (
    <div class="session-row">
      <Button
        class={`session ${selected ? "selected" : ""}`}
        onClick={() => choose(item.id)}
        aria-current={selected ? "page" : undefined}
      >
        <span>
          <SessionName item={item} />
          {unread && <span class="unread-dot" aria-label="Unread messages" />}
        </span>
        <small>
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
        </small>
      </Button>
      {menu(item)}
    </div>
  );
}

export default function Sidebar({
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
}: {
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
}) {
  const [search, setSearch] = useState("");
  // Until the list arrives, it draws sample rows in its own layout.
  const drawing = loading && !sessions.length;
  const all = drawing ? SAMPLE : sessions;
  // Each folder's coordinator is its header icon, not a row. It and its
  // badge count every session in the folder, whatever the search shows.
  const coordinators = new Map<string, Session>();
  const waiting = new Map<string, number>();
  for (const item of all) {
    const folder = folderOf(item);
    if (item.kind === "coordinator") coordinators.set(folder, item);
    if (item.pending) waiting.set(folder, (waiting.get(folder) || 0) + 1);
  }
  const groups = new Map<string, Session[]>();
  for (const item of [...all]
    .sort((a, b) => (b.updated || 0) - (a.updated || 0))
    .filter((item) =>
      `${item.title} ${item.cwd}`.toLowerCase().includes(search.toLowerCase()),
    )) {
    const folder = folderOf(item);
    // A folder with only its coordinator still has a header to open it.
    if (!groups.has(folder)) groups.set(folder, []);
    if (item.kind !== "coordinator") groups.get(folder)!.push(item);
  }
  const list = [...groups].map(([cwd, items]) => (
    <section key={cwd}>
      <h2>
        <FolderLabel path={cwd} />
        {!drawing && (
          <CoordinatorButton
            folder={cwd}
            coordinator={coordinators.get(cwd)}
            waiting={waiting.get(cwd) || 0}
            online={online}
            open={() => coordinate(cwd)}
          />
        )}
      </h2>
      {items.map((item) => (
        <SessionRow
          key={item.id}
          item={item}
          selected={item.id === selected}
          unread={unread.has(item.id)}
          online={online}
          choose={choose}
          menu={menu}
        />
      ))}
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
        <Button
          variant="quiet"
          class="with-icon"
          onClick={create}
          disabled={!online}
        >
          <Plus />
          New conversation
        </Button>
      </div>
      <div class="sidebar-sections">
        <Button
          variant="quiet"
          class="with-icon"
          aria-current={page === "library" ? "page" : undefined}
          onClick={() => navigate("library")}
        >
          <Library />
          Library
        </Button>
        <Button
          variant="quiet"
          class="with-icon"
          aria-current={page === "scheduled" ? "page" : undefined}
          onClick={() => navigate("scheduled")}
        >
          <CalendarClock />
          Scheduled
          {scheduledUnread && (
            <span class="unread-dot" aria-label="Unread scheduled results" />
          )}
        </Button>
      </div>
      <label class="search">
        <span class="sr-only">Find a session</span>
        <Input
          id="session-search"
          type="search"
          value={search}
          onInput={(event) => setSearch(event.currentTarget.value)}
          placeholder="Find a conversation…"
        />
      </label>
      <nav>
        <Placeholder label="Loading conversations…" when={drawing}>
          {list}
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
