import { ConnectionStatus } from "../../shared/connection-status.tsx";
import { Fragment, type ComponentChildren } from "preact";
import { useMemo } from "preact/hooks";
import type { Session, Space, TerminalInfo } from "../../shared/types.ts";
import {
  ChevronDown,
  ChevronRight,
  Plus,
  RefreshCw,
  Settings,
  Library,
  CalendarClock,
  Search,
  CircleAlert,
  TriangleAlert,
  Inbox,
  Terminal,
} from "lucide-preact";
import { useHostContext } from "../../state/host-context.ts";
import {
  Mark,
  Time,
  Button,
  IconButton,
  Placeholder,
  EmptyState,
  updateChecked,
} from "../../shared/ui.tsx";
import FolderLabel, { folderName } from "../../shared/folder-label.tsx";
import { folderOf } from "../../shared/display.ts";
import SessionName from "../../shared/session-name.tsx";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import { plural } from "../../shared/quantities.ts";
import { statusOf, type SessionState } from "../../state/attention.ts";
import { SheetButton } from "../../shared/sheet.tsx";
import WaitingList from "../../shared/escalations.tsx";
import { collapsed, unfolded, useFolding } from "./folding.ts";
import "./sidebar.css";

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

// What a row says of its conversation, at its right edge: it waits on you,
// it failed, or it works (the LED breathes). An idle one says nothing.
const STATES: Partial<
  Record<SessionState, { label: string; Icon?: typeof Inbox }>
> = {
  waiting: { label: "Needs your input", Icon: CircleAlert },
  failed: { label: "Failed", Icon: TriangleAlert },
  working: { label: "Working" },
};

// A conversation and the agents it started, which are listed beneath it:
// those at work (or waiting on you) one by one, the finished ones folded
// into one line until it is opened.
type Entry = { item: Session; working: Entry[]; finished: Entry[] };

// The list's places in its order, newest first, each with its
// conversations. An agent whose parent is not listed is an ordinary row. A
// workspace is a place whether or not anything is in it yet.
function groupSessions(
  sessions: Session[],
  spaces: Space[] = [],
  online = true,
) {
  const groups = new Map<string, Entry[]>();
  const place = (path: string) => {
    if (!groups.has(path)) groups.set(path, []);
    return groups.get(path)!;
  };
  const sorted = [...sessions].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  );
  const listed = new Set(sorted.map((item) => item.id));
  const entry = (item: Session): Entry => {
    const agents = sorted
      .filter((agent) => agent.parent === item.id)
      .map(entry);
    const idle = (agent: Entry) => statusOf(agent.item, online) === "idle";
    return {
      item,
      working: agents.filter((agent) => !idle(agent)),
      finished: agents.filter(idle),
    };
  };
  for (const item of sorted)
    if (!item.parent || !listed.has(item.parent))
      place(folderOf(item)).push(entry(item));
  for (const space of spaces) place(space.path);
  return groups;
}

// The rows a conversation shows, in order: itself, its working agents, and
// its finished ones once unfolded.
const rowsOf = ({ item, working, finished }: Entry): Session[] => [
  item,
  ...working.flatMap(rowsOf),
  ...(unfolded.has(item.id) ? finished.flatMap(rowsOf) : []),
];

// The places under the word for their kind, workspaces first: one is
// µAgent's to keep and remove, the other a directory of yours.
function placesByKind(
  sessions: Session[],
  spaces: Space[] = [],
  online = true,
) {
  const places = [...groupSessions(sessions, spaces, online)];
  const space = (path: string) => spaces.some((item) => item.path === path);
  return (
    [
      ["Workspaces", places.filter(([path]) => space(path))],
      ["Folders", places.filter(([path]) => !space(path))],
    ] as const
  ).filter(([, shown]) => shown.length);
}

// The rows in the order the list shows them: what Alt+↑/↓ steps through.
export const sessionOrder = (
  sessions: Session[],
  spaces: Space[] | undefined,
  online = true,
) =>
  placesByKind(sessions, spaces, online).flatMap(([, places]) =>
    places.flatMap(([path, entries]) =>
      collapsed.has(path) ? [] : entries.flatMap(rowsOf),
    ),
  );

type SessionRowProps = {
  item: Session;
  selected: boolean;
  unread: boolean;
  online: boolean;
  choose: (id: string) => void;
  menu: (session: Session) => ComponentChildren;
};

// One conversation in the list, on one line: its title and its state. The
// time and the menu show while the row is pointed at or focused.
function SessionRow({
  item,
  selected,
  unread,
  online,
  choose,
  menu,
}: SessionRowProps) {
  const state = statusOf(item, online);
  const shown = STATES[state];
  return (
    <div class="session-row entry">
      <ListRow
        class="session"
        onClick={() => choose(item.id)}
        aria-current={selected ? "page" : undefined}
        title={<SessionName item={item} />}
        unread={unread && "Unread messages"}
      >
        {item.updated ? (
          <span class="session-time">
            <Time value={item.updated} />
          </span>
        ) : null}
        {shown && (
          <span
            class={`session-state ${state}`}
            role="img"
            aria-label={shown.label}
          >
            {shown.Icon ? (
              <shown.Icon aria-hidden />
            ) : (
              <StatusLed state="running" />
            )}
          </span>
        )}
      </ListRow>
      {menu(item)}
    </div>
  );
}

// A patch to one session keeps the others' objects, so only its row renders.
const SessionEntry = updateChecked(SessionRow);

type SidebarProps = {
  loading: boolean;
  page: string;
  navigate: (page: "chat" | "library" | "scheduled") => void;
  scheduledUnread: boolean;
  sessions: Session[];
  // How many of them wait on a decision.
  needsYou: number;
  spaces: Space[];
  // What can be done to a workspace: rename it, delete it.
  spaceMenu: (space: Space, conversations: number) => ComponentChildren;
  selected: string;
  unread: Set<string>;
  choose: (id: string) => void;
  menu: (session: Session) => ComponentChildren;
  settings: () => void;
  // Asks for a workspace's name or a folder's path, then starts there.
  create: (place: "workspace" | "folder") => void;
  // A new conversation in a place, and the place's overview.
  start: (cwd: string) => void;
  // A new conversation in the Scratch workspace, with no folder chosen.
  startScratch: () => void;
  overview: (cwd: string) => void;
  // The place whose overview is open.
  shownPlace: string;
  // The folders' terminals, the one shown, and opening one; a new one only
  // while the host offers them.
  terminals?: TerminalInfo[] | null;
  shownTerminal?: string;
  showTerminal: (terminal: TerminalInfo) => void;
  newTerminal?: (folder: string) => void;
  // Search over every conversation, command and setting.
  search: () => void;
};

function SidebarView({
  loading,
  sessions,
  needsYou,
  spaces,
  spaceMenu,
  selected,
  unread,
  choose,
  menu,
  settings,
  create,
  start,
  startScratch,
  overview,
  shownPlace,
  terminals,
  shownTerminal,
  showTerminal,
  newTerminal,
  page,
  navigate,
  scheduledUnread,
  search,
}: SidebarProps) {
  const { online, connection, refresh } = useHostContext();
  useFolding();
  // Until the list arrives, it draws sample rows in its own layout.
  const drawing = loading && !sessions.length;
  const all = drawing ? SAMPLE : sessions;
  // Grouped again only when the list, the places or the search change, not
  // for a selection, an unread mark or a fold.
  const places = useMemo(
    () => placesByKind(all, drawing ? [] : spaces, online),
    [all, drawing, spaces, online],
  );
  // How many conversations each place holds, whatever the search shows.
  const counts = useMemo(() => {
    const counts = new Map<string, number>();
    for (const item of all)
      counts.set(folderOf(item), (counts.get(folderOf(item)) || 0) + 1);
    return counts;
  }, [all]);
  const row = (item: Session) => (
    <SessionEntry
      key={item.id}
      item={item}
      selected={item.id === selected}
      unread={unread.has(item.id)}
      online={online}
      choose={choose}
      menu={menu}
    />
  );
  // A conversation's rows: itself, then its agents beneath it.
  const conversation = (entry: Entry) => {
    const { item, working, finished } = entry;
    const open = unfolded.has(item.id);
    return (
      <Fragment key={item.id}>
        {row(item)}
        {(working.length > 0 || finished.length > 0) && (
          <div
            class="place-agents"
            role="group"
            aria-label={`Agents of ${item.title || "this conversation"}`}
          >
            {working.map(conversation)}
            {finished.length > 0 && (
              <Button
                variant="quiet"
                class="agents-fold entry small"
                aria-expanded={open}
                onClick={() => unfolded.set(item.id, !open)}
              >
                {open ? <ChevronDown /> : <ChevronRight />}
                {plural(finished.length, "agent")}
              </Button>
            )}
            {open && finished.map(conversation)}
          </div>
        )}
      </Fragment>
    );
  };
  const kept = new Map(spaces.map((space) => [space.path, space]));
  const place = ([cwd, entries]: [string, Entry[]]) => {
    const space = kept.get(cwd);
    const name = space?.name || folderName(cwd);
    const folded = collapsed.has(cwd);
    const conversations = entries.flatMap(function all(entry): Session[] {
      return [
        entry.item,
        ...entry.working.flatMap(all),
        ...entry.finished.flatMap(all),
      ];
    });
    const shells = (terminals || []).filter((item) => item.folder === cwd);
    const holdsOpen = conversations.some((item) => item.id === selected);
    const attention = conversations.filter((item) =>
      ["waiting", "failed"].includes(statusOf(item, online)),
    ).length;
    return (
      <section key={cwd}>
        <div class="place-head entry">
          <IconButton
            label={`Conversations of ${name}`}
            aria-expanded={!folded}
            aria-current={folded && holdsOpen ? "true" : undefined}
            onClick={(event) => {
              // Focus stays on the header, not in the rows it takes away.
              event.currentTarget.focus();
              collapsed.set(cwd, !folded);
            }}
          >
            {folded ? <ChevronRight /> : <ChevronDown />}
          </IconButton>
          <h2>
            <Button
              variant="quiet"
              title={`Overview of ${name}`}
              disabled={drawing}
              aria-current={shownPlace === cwd ? "page" : undefined}
              onClick={() => overview(cwd)}
            >
              <FolderLabel path={cwd} space={!!space}>
                {space?.name}
              </FolderLabel>
              {folded && conversations.length > 0 && (
                <span
                  class={`place-count${attention ? " attention" : ""}`}
                  aria-label={
                    attention
                      ? `${attention} need attention`
                      : plural(conversations.length, "conversation")
                  }
                >
                  ({attention || conversations.length})
                </span>
              )}
            </Button>
          </h2>
          <Menu
            label={`New in ${name}`}
            trigger={
              <>
                <Plus />
                <span class="sr-only">New in {name}</span>
              </>
            }
          >
            <MenuItem
              disabled={!online}
              onClick={() => {
                collapsed.set(cwd, false);
                start(cwd);
              }}
            >
              Conversation
            </MenuItem>
            {newTerminal && (
              <MenuItem disabled={!online} onClick={() => newTerminal(cwd)}>
                Terminal
              </MenuItem>
            )}
          </Menu>
          {space && spaceMenu(space, counts.get(cwd) || 0)}
        </div>
        {!folded && (
          <div class="place-body">
            {shells.map((terminal, index) => (
              <div class="session-row entry" key={terminal.id}>
                <ListRow
                  class="session"
                  aria-current={
                    terminal.id === shownTerminal ? "page" : undefined
                  }
                  onClick={() => showTerminal(terminal)}
                  title={
                    <>
                      <Terminal class="row-icon" aria-hidden="true" />
                      Terminal{shells.length > 1 ? ` ${index + 1}` : ""}
                    </>
                  }
                />
              </div>
            ))}
            {entries.map(conversation)}
          </div>
        )}
      </section>
    );
  };
  const list = places.map(([kind, shown]) => (
    <div key={kind} class="list-group" role="group" aria-label={kind}>
      <p class="section-title caption">{kind}</p>
      {shown.map(place)}
    </div>
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
        <IconButton label="Search" onClick={search}>
          <Search />
        </IconButton>
        <Menu
          label="New"
          trigger={
            <>
              New
              <ChevronDown />
            </>
          }
        >
          <MenuItem disabled={!online} onClick={startScratch}>
            Conversation
          </MenuItem>
          <MenuItem disabled={!online} onClick={() => create("workspace")}>
            Workspace…
          </MenuItem>
          <MenuItem disabled={!online} onClick={() => create("folder")}>
            Folder…
          </MenuItem>
        </Menu>
      </div>
      {needsYou > 0 && (
        <SheetButton
          label={`${needsYou} need${needsYou === 1 ? "s" : ""} you`}
          heading="Needs you"
          className="needs-you"
          trigger={
            <>
              <Inbox />
              {needsYou} need{needsYou === 1 ? "s" : ""} you
            </>
          }
        >
          {(close) => (
            <WaitingList
              sessions={all}
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
          class="entry"
          aria-current={page === "library" ? "page" : undefined}
          onClick={() => navigate("library")}
        >
          <Library />
          Library
        </Button>
        <Button
          variant="quiet"
          class="entry"
          aria-current={page === "scheduled" ? "page" : undefined}
          onClick={() => navigate("scheduled")}
        >
          <CalendarClock />
          Scheduled
          {scheduledUnread && (
            <span
              class="status-led unread"
              role="img"
              aria-label="Unread scheduled results"
            />
          )}
        </Button>
      </div>
      <nav aria-label="Conversations">
        <Placeholder label="Loading conversations…" when={drawing}>
          {list.length ? (
            list
          ) : (
            <EmptyState>
              No conversations yet. New starts one in a workspace or in a folder
              on your host.
            </EmptyState>
          )}
        </Placeholder>
      </nav>
      <footer>
        <ConnectionStatus phase={connection} className="connection small" />
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
export default updateChecked(SidebarView);
