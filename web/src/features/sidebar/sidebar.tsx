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
  agents: Session[];
  others: Session[];
};

// The list's places in its order, newest first: each one's panel, the
// agents it started, then its conversations, each followed by its own
// agents. An agent whose parent is not listed is an ordinary row. A
// workspace is a place whether or not anything is in it yet.
function groupSessions(sessions: Session[], spaces: Space[] = []) {
  const groups = new Map<string, Place>();
  const place = (path: string) => {
    if (!groups.has(path)) groups.set(path, { agents: [], others: [] });
    return groups.get(path)!;
  };
  const sorted = [...sessions].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  );
  const listed = new Set(sorted.map((item) => item.id));
  const agents = (item: Session): Session[] =>
    sorted
      .filter((agent) => agent.parent === item.id)
      .flatMap((agent) => [agent, ...agents(agent)]);
  for (const item of sorted) {
    if (item.parent && listed.has(item.parent)) continue;
    const path = folderOf(item);
    if (item.kind === "panel") {
      place(path).panel = item;
      place(path).agents = agents(item);
    } else place(path).others.push(item, ...agents(item));
  }
  for (const space of spaces) place(space.path);
  return groups;
}

// The places under the word for their kind, workspaces first: one is
// µAgent's to keep and remove, the other a directory of yours.
function placesByKind(sessions: Session[], spaces: Space[] = []) {
  const places = [...groupSessions(sessions, spaces)];
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
) =>
  placesByKind(sessions, spaces).flatMap(([, places]) =>
    places.flatMap(([path, group]) =>
      collapsed.has(path)
        ? []
        : [
            ...(group.panel ? [group.panel] : []),
            ...group.agents,
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
  openPanel,
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
    () => placesByKind(all, drawing ? [] : spaces),
    [all, drawing, spaces],
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
    const folded = collapsed.has(cwd);
    const { panel } = group;
    const conversations = [...group.agents, ...group.others];
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
              title={`Lead agent of ${name}`}
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
            {group.agents.length > 0 && (
              <div role="group" aria-label={`Agents of ${name}`}>
                {group.agents.map(row)}
              </div>
            )}
            {group.agents.length > 0 && group.others.length > 0 && (
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
