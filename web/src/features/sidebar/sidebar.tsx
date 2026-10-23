import { ConnectionStatus } from "../../shared/connection-status.tsx";
import type { ComponentChildren } from "preact";
import { useMemo } from "preact/hooks";
import type { Session, Space } from "../../shared/types.ts";
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
} from "lucide-preact";
import { useHostContext } from "../../state/host-context.ts";
import {
  Mark,
  Input,
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
import { collapsed, useFolding } from "./folding.ts";
import { matches } from "../../shared/fuzzy.ts";
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

type Place = {
  panel?: Session;
  threads: Session[];
  others: Session[];
};

// The list's places in its order, newest first: each one's panel, what
// the panel started, then its conversations. A workspace is a place whether
// or not anything is in it yet.
function groupSessions(sessions: Session[], spaces: Space[] = [], search = "") {
  const groups = new Map<string, Place>();
  const place = (path: string) => {
    if (!groups.has(path)) groups.set(path, { threads: [], others: [] });
    return groups.get(path)!;
  };
  const names = new Map(spaces.map((space) => [space.path, space.name]));
  for (const item of [...sessions].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  )) {
    const path = folderOf(item);
    // A panel is listed with its place, whatever the search shows of it.
    if (item.kind === "panel") {
      if (matches(search, names.get(path), path)) place(path).panel = item;
    } else if (matches(search, item.title, names.get(path), item.cwd)) {
      (item.kind === "thread" ? place(path).threads : place(path).others).push(
        item,
      );
    }
  }
  for (const space of spaces)
    if (matches(search, space.name)) place(space.path);
  return groups;
}

// The places under the word for their kind, workspaces first: one is
// µAgent's to keep and remove, the other a directory of yours.
function placesByKind(sessions: Session[], spaces: Space[] = [], search = "") {
  const places = [...groupSessions(sessions, spaces, search)];
  const space = (path: string) => spaces.some((item) => item.path === path);
  return (
    [
      ["Workspaces", places.filter(([path]) => space(path))],
      ["Folders", places.filter(([path]) => !space(path))],
    ] as const
  ).filter(([, shown]) => shown.length);
}

// A search shows what it found, whatever is folded.
const foldedOf = (path: string, query: string) => !query && collapsed.has(path);

// The rows in the order the list shows them: what Alt+↑/↓ steps through.
export const sessionOrder = (
  sessions: Session[],
  spaces: Space[] | undefined,
  query: string,
) =>
  placesByKind(sessions, spaces, query).flatMap(([, places]) =>
    places.flatMap(([path, group]) =>
      foldedOf(path, query)
        ? []
        : [
            ...(group.panel ? [group.panel] : []),
            ...group.threads,
            ...group.others,
          ],
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
    <div class="session-row">
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
  // What the list is filtered by: it decides which rows show.
  query: string;
  setSearch: (query: string) => void;
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
  // A new conversation in a place, and the place's panel.
  start: (cwd: string) => void;
  openPanel: (cwd: string) => void;
  palette: () => void;
};

function SidebarView({
  loading,
  sessions,
  needsYou,
  query,
  setSearch,
  spaces,
  spaceMenu,
  selected,
  unread,
  choose,
  menu,
  settings,
  create,
  start,
  openPanel,
  page,
  navigate,
  scheduledUnread,
  palette,
}: SidebarProps) {
  const { online, connection, refresh } = useHostContext();
  useFolding();
  // Until the list arrives, it draws sample rows in its own layout.
  const drawing = loading && !sessions.length;
  const all = drawing ? SAMPLE : sessions;
  // Grouped again only when the list, the places or the search change, not
  // for a selection, an unread mark or a fold.
  const places = useMemo(
    () => placesByKind(all, drawing ? [] : spaces, query),
    [all, drawing, spaces, query],
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
  const kept = new Map(spaces.map((space) => [space.path, space]));
  const place = ([cwd, group]: [string, Place]) => {
    const space = kept.get(cwd);
    const name = space?.name || folderName(cwd);
    const folded = foldedOf(cwd, query);
    const { panel } = group;
    const conversations = [...group.threads, ...group.others];
    const holdsOpen = [panel, ...conversations].some(
      (item) => item?.id === selected,
    );
    const attention = [panel, ...conversations].filter(
      (item) => item && ["waiting", "failed"].includes(statusOf(item, online)),
    ).length;
    return (
      <section key={cwd}>
        <div class="place-head">
          <IconButton
            label={`Conversations of ${name}`}
            aria-expanded={!folded}
            aria-current={
              folded && holdsOpen && !(panel && selected === panel.id)
                ? "true"
                : undefined
            }
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
              title={`Panel of ${name}`}
              disabled={drawing || (!panel && !online)}
              aria-current={panel && selected === panel.id ? "page" : undefined}
              onClick={() => (panel ? choose(panel.id) : openPanel(cwd))}
            >
              <FolderLabel path={cwd} space={!!space}>
                {space?.name}
              </FolderLabel>
              {panel && unread.has(panel.id) && (
                <span
                  class="unread-dot"
                  role="img"
                  aria-label="Unread messages"
                />
              )}
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
          <IconButton
            label={`New conversation in ${name}`}
            disabled={!online}
            onClick={() => {
              collapsed.set(cwd, false);
              start(cwd);
            }}
          >
            <Plus />
          </IconButton>
          {space && spaceMenu(space, counts.get(cwd) || 0)}
        </div>
        {!folded && (
          <div class="place-body">
            {group.threads.length > 0 && (
              <div role="group" aria-label={`Threads of ${name}`}>
                {group.threads.map(row)}
              </div>
            )}
            {group.threads.length > 0 && group.others.length > 0 && (
              <hr class="place-rule" />
            )}
            {group.others.map(row)}
          </div>
        )}
      </section>
    );
  };
  const list = places.map(([kind, shown]) => (
    <div key={kind} class="list-group" role="group" aria-label={kind}>
      <p class="section-title">{kind}</p>
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
        <IconButton label="Command palette" onClick={palette}>
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
          value={query}
          onInput={(event) => setSearch(event.currentTarget.value)}
          placeholder="Find a conversation…"
        />
      </label>
      <nav aria-label="Conversations">
        <Placeholder label="Loading conversations…" when={drawing}>
          {list.length ? (
            list
          ) : (
            <EmptyState>
              {query
                ? "No conversation matches."
                : "No conversations yet. New starts one in a workspace or in a folder on your host."}
            </EmptyState>
          )}
        </Placeholder>
      </nav>
      <footer>
        <ConnectionStatus phase={connection} className="connection" />
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
