import { useHosts } from "../../state/sources.ts";
import { Fold, SharedShortcuts, OtherPlaces, HostList } from "./hosts.tsx";
import { ConnectionStatus } from "../../shared/connection-status.tsx";
import { Fragment, type ComponentChildren } from "preact";
import { useCallback, useEffect, useMemo, useRef } from "preact/hooks";
import type { Pin, Session, Space, TerminalInfo } from "../../shared/types.ts";
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
import { Menu, MenuItem, useLongPress } from "../../shared/menu.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import { plural } from "../../shared/quantities.ts";
import { statusOf, type SessionState } from "../../state/attention.ts";
import { SheetButton } from "../../shared/sheet.tsx";
import WaitingList from "../../shared/escalations.tsx";
import { collapsed, placeKey, useFolding } from "./folding.ts";
import { PinItem } from "./pins.tsx";
import type { SelectionApi } from "../../shared/selection.ts";
import { useLatest } from "../../shared/use-latest.ts";
import { useShortcuts } from "../../shared/shortcuts.ts";
import { RowCheck, SelectionBar } from "../../shared/selection-bar.tsx";
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

// A conversation and its agents at work (or waiting on you), listed beneath
// it. A finished agent leaves the list: its report in the conversation opens
// it, and search finds it.
type Entry = { item: Session; agents: Entry[] };

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
  const entry = (item: Session): Entry => ({
    item,
    agents: sorted
      .filter(
        (agent) =>
          agent.parent === item.id && statusOf(agent, online) !== "idle",
      )
      .map(entry),
  });
  for (const item of sorted)
    if (!item.parent || !listed.has(item.parent))
      place(folderOf(item)).push(entry(item));
  for (const space of spaces) place(space.path);
  return groups;
}

// The rows a conversation shows, in order: itself, then its agents at work.
const rowsOf = ({ item, agents }: Entry): Session[] => [
  item,
  ...agents.flatMap(rowsOf),
];

// The pinned conversations, folders and terminals in pin order, then the
// other places under the word for their kind, workspaces first: one is
// µAgent's to keep and remove, the other a directory of yours. A pinned
// conversation or folder is listed once, among the pinned.
type Place = [string, Entry[]];
type Pinned = { entry: Entry } | { place: Place } | { terminal: TerminalInfo };

function arrange(
  sessions: Session[],
  spaces: Space[] = [],
  online = true,
  pins: Pin[] = [],
  terminals: TerminalInfo[] = [],
) {
  const groups = [...groupSessions(sessions, spaces, online)];
  const space = (path: string) => spaces.some((item) => item.path === path);
  const pinned = (kind: Pin["kind"], id: string) =>
    pins.some((pin) => pin.kind === kind && pin.id === id);
  const entries = groups.flatMap(([, entries]) => entries);
  const places = groups
    .map(
      ([path, entries]): Place => [
        path,
        entries.filter((entry) => !pinned("session", entry.item.id)),
      ],
    )
    .filter(([path, entries]) => entries.length || space(path));
  const top = pins.flatMap((pin): Pinned[] => {
    const entry = entries.find((entry) => entry.item.id === pin.id);
    const place = places.find(([path]) => path === pin.id);
    if (pin.kind === "terminal") {
      const terminal = terminals.find((item) => item.id === pin.id);
      return terminal ? [{ terminal }] : [];
    }
    if (pin.kind === "session") return entry ? [{ entry }] : [];
    return place ? [{ place }] : [];
  });
  const rest = places.filter(([path]) => !pinned("folder", path));
  return {
    top,
    kinds: (
      [
        ["Workspaces", rest.filter(([path]) => space(path))],
        ["Folders", rest.filter(([path]) => !space(path))],
      ] as const
    ).filter(([, shown]) => shown.length),
  };
}

// The rows in the order the list shows them: what Alt+↑/↓ steps through.
export const sessionOrder = (
  sessions: Session[],
  spaces: Space[] | undefined,
  online = true,
  pins: Pin[] = [],
  host = "",
) => {
  const { top, kinds } = arrange(sessions, spaces, online, pins);
  const rows = ([path, entries]: Place) =>
    collapsed.has(placeKey(host, path)) ? [] : entries.flatMap(rowsOf);
  return [
    ...(collapsed.has("$pinned") ? [] : top).flatMap((pinned) =>
      "entry" in pinned
        ? rowsOf(pinned.entry)
        : "place" in pinned
          ? rows(pinned.place)
          : [],
    ),
    ...(collapsed.has(`$host:${host}`) ? [] : kinds).flatMap(([, places]) =>
      places.flatMap(rows),
    ),
  ];
};

type SessionRowProps = {
  item: Session;
  selected: boolean;
  unread: boolean;
  online: boolean;
  choose: (id: string) => void;
  menu: (session: Session) => ComponentChildren;
  // Picked to act on with others; while picking, its checkbox shows.
  picked: boolean;
  selecting: boolean;
  pick: (id: string, event: MouseEvent) => boolean;
  toggle: (id: string) => void;
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
  picked,
  selecting,
  pick,
  toggle,
}: SessionRowProps) {
  const state = statusOf(item, online);
  const shown = STATES[state];
  return (
    <div class="session-row entry" data-picked={picked || undefined}>
      {selecting && (
        <RowCheck
          picked={picked}
          label={`Select ${item.title || "Untitled"}`}
          toggle={() => toggle(item.id)}
        />
      )}
      <ListRow
        class="session"
        onClick={(event) => pick(item.id, event) || choose(item.id)}
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

// One folder terminal in the list, as a conversation is: its name and start
// time, and the same menu on hover or focus.
function TerminalRow({
  terminal,
  selected,
  choose,
  menu,
}: {
  terminal: TerminalInfo;
  selected: boolean;
  choose: (terminal: TerminalInfo) => void;
  menu: (terminal: TerminalInfo) => ComponentChildren;
}) {
  return (
    <div class="session-row entry">
      <ListRow
        class="session"
        aria-current={selected ? "page" : undefined}
        onClick={() => choose(terminal)}
        title={
          <>
            <Terminal class="row-icon" aria-hidden="true" />
            {terminal.title}
          </>
        }
      >
        <span class="session-time">
          <Time value={terminal.started} />
        </span>
      </ListRow>
      {menu(terminal)}
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
  // The pinned conversations, folders and terminals, in order.
  pins: Pin[];
  // What can be done to a workspace: rename it, delete it.
  spaceMenu: (space: Space, conversations: number) => ComponentChildren;
  // Conversations and workspaces picked to act on together, and the act.
  selection: SelectionApi;
  act: (
    action: "move" | "close" | "delete",
    sessions: Session[],
    spaces: Space[],
  ) => void;
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
  terminalMenu: (terminal: TerminalInfo) => ComponentChildren;
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
  pins,
  spaceMenu,
  selection,
  act,
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
  terminalMenu,
  showTerminal,
  newTerminal,
  page,
  navigate,
  scheduledUnread,
  search,
}: SidebarProps) {
  const { online, connection, refresh } = useHostContext();
  const hosts = useHosts();
  const host = hosts?.active || "";
  useFolding();
  const rows = useRef<HTMLElement>(null);
  useLongPress(rows, ".session-row, .place-head");
  // Until the list arrives, it draws sample rows in its own layout.
  const drawing = loading && !sessions.length;
  const all = drawing ? SAMPLE : sessions;
  // Grouped again only when the list, the places or the search change, not
  // for a selection, an unread mark or a fold.
  const { top, kinds } = useMemo(
    () => arrange(all, drawing ? [] : spaces, online, pins, terminals || []),
    [all, drawing, spaces, online, pins, terminals],
  );
  // What can be picked, in the order the list shows it: each workspace,
  // then the conversations of each place that is open.
  const pickable = ([path, entries]: Place) => [
    ...(spaces.some((space) => space.path === path) ? [path] : []),
    ...(collapsed.has(placeKey(host, path))
      ? []
      : entries.flatMap(rowsOf).map((s) => s.id)),
  ];
  const order = (() => {
    return [
      ...(collapsed.has("$pinned") ? [] : top).flatMap((pinned) =>
        "entry" in pinned
          ? rowsOf(pinned.entry).map((item) => item.id)
          : "place" in pinned
            ? pickable(pinned.place)
            : [],
      ),
      ...(collapsed.has(`$host:${host}`) ? [] : kinds).flatMap(([, places]) =>
        places.flatMap(pickable),
      ),
    ];
  })();
  // A conversation or workspace gone (deleted, moved) leaves the selection.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on a changed list
  useEffect(() => {
    if (selection.ids.size)
      selection.keep([
        ...all.map((item) => item.id),
        ...spaces.map((space) => space.path),
      ]);
  }, [all, spaces]);
  const latest = useLatest({ selection, order });
  const pick = useCallback(
    (id: string, event: MouseEvent) =>
      latest.current.selection.click(id, event, latest.current.order),
    [latest],
  );
  const toggle = useCallback(
    (id: string) => latest.current.selection.toggle(id),
    [latest],
  );
  const picked = {
    sessions: all.filter((item) => selection.ids.has(item.id)),
    spaces: spaces.filter((space) => selection.ids.has(space.path)),
  };
  // Keys while selecting, wherever the focus is (Safari focuses no clicked
  // row): ⌘A all, ⌫ deletes, Esc stops.
  useShortcuts(
    selection.selecting
      ? {
          "select-all": () => selection.all(order),
          "delete-selected": selection.ids.size
            ? () => act("delete", picked.sessions, picked.spaces)
            : undefined,
          deselect: selection.clear,
        }
      : {},
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
      picked={selection.ids.has(item.id)}
      selecting={selection.selecting}
      pick={pick}
      toggle={toggle}
    />
  );
  // A conversation's rows: itself, then its agents at work beneath it.
  const conversation = ({ item, agents }: Entry) => (
    <Fragment key={item.id}>
      {row(item)}
      {agents.length > 0 && (
        <div
          class="place-agents"
          role="group"
          aria-label={`Agents of ${item.title || "this conversation"}`}
        >
          {agents.map(conversation)}
        </div>
      )}
    </Fragment>
  );
  const terminalRow = (terminal: TerminalInfo) => (
    <TerminalRow
      key={terminal.id}
      terminal={terminal}
      selected={terminal.id === shownTerminal}
      choose={showTerminal}
      menu={terminalMenu}
    />
  );
  const kept = new Map(spaces.map((space) => [space.path, space]));
  const place = ([cwd, entries]: Place) => {
    const space = kept.get(cwd);
    const name = space?.name || folderName(cwd);
    const folded = collapsed.has(placeKey(host, cwd));
    const conversations = entries.flatMap(function all(entry): Session[] {
      return [entry.item, ...entry.agents.flatMap(all)];
    });
    const shells = (terminals || []).filter(
      (item) =>
        item.folder === cwd &&
        !pins.some((pin) => pin.kind === "terminal" && pin.id === item.id),
    );
    const holdsOpen = conversations.some((item) => item.id === selected);
    const attention = conversations.filter((item) =>
      ["waiting", "failed"].includes(statusOf(item, online)),
    ).length;
    return (
      <section key={cwd}>
        <div
          class="place-head entry"
          data-picked={selection.ids.has(cwd) || undefined}
        >
          {selection.selecting && space && (
            <RowCheck
              picked={selection.ids.has(cwd)}
              label={`Select the workspace ${name}`}
              toggle={() => toggle(cwd)}
            />
          )}
          <IconButton
            label={`Conversations of ${name}`}
            aria-expanded={!folded}
            aria-current={folded && holdsOpen ? "true" : undefined}
            onClick={(event) => {
              // Focus stays on the header, not in the rows it takes away.
              event.currentTarget.focus();
              collapsed.set(placeKey(host, cwd), !folded);
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
              onClick={(event) => (space && pick(cwd, event)) || overview(cwd)}
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
                collapsed.set(placeKey(host, cwd), false);
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
          {space ? (
            spaceMenu(space, counts.get(cwd) || 0)
          ) : (
            <Menu label={`Folder menu for ${name}`}>
              <PinItem pin={{ kind: "folder", id: cwd }} />
            </Menu>
          )}
        </div>
        {!folded && (
          <div class="place-body">
            {shells.map(terminalRow)}
            {entries.map(conversation)}
          </div>
        )}
      </section>
    );
  };
  const group = (kind: string, rows: ComponentChildren[]) => (
    <div key={kind} class="list-group" role="group" aria-label={kind}>
      <p class="section-title caption">{kind}</p>
      {rows}
    </div>
  );
  const activePins = top.map((pinned) =>
    "entry" in pinned
      ? conversation(pinned.entry)
      : "place" in pinned
        ? place(pinned.place)
        : terminalRow(pinned.terminal),
  );
  const list = [
    <SharedShortcuts key="shortcuts" activePins={activePins} />,
    ...(hosts
      ? hosts.sources.map((source) =>
          source.id === host ? (
            <Fold key={source.id} name={`$host:${host}`} label={source.name}>
              {kinds.map(([kind, shown]) => group(kind, shown.map(place)))}
              {!kinds.some(([, shown]) => shown.length) &&
                !activePins.length && (
                  <EmptyState>
                    No conversations yet. New starts one in a workspace or
                    folder on this host.
                  </EmptyState>
                )}
            </Fold>
          ) : (
            <OtherPlaces key={source.id} source={source} />
          ),
        )
      : kinds.map(([kind, shown]) => group(kind, shown.map(place)))),
  ];
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
      <nav
        ref={rows}
        aria-label="Conversations"
        data-selecting={selection.selecting || undefined}
      >
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
      <HostList />
      {selection.selecting ? (
        <footer>
          <SelectionBar selection={selection}>
            <Button
              variant="secondary"
              disabled={
                !online || !picked.sessions.length || picked.spaces.length > 0
              }
              onClick={() => act("move", picked.sessions, [])}
            >
              Move to…
            </Button>
            <Button
              variant="secondary"
              disabled={
                !online || !picked.sessions.some((item) => item.generation)
              }
              onClick={() => act("close", picked.sessions, [])}
            >
              Close
            </Button>
            <Button
              variant="destructive"
              disabled={!online || !selection.ids.size}
              onClick={() => act("delete", picked.sessions, picked.spaces)}
            >
              Delete…
            </Button>
          </SelectionBar>
        </footer>
      ) : (
        <footer>
          <ConnectionStatus phase={connection} className="connection small" />
          <IconButton label="Refresh" onClick={refresh}>
            <RefreshCw />
          </IconButton>
          <IconButton label="Settings" onClick={settings}>
            <Settings />
          </IconButton>
        </footer>
      )}
    </>
  );
}

// The shell re-renders with every streamed frame; the list only when one of
// its props changes, which the shell keeps stable.
export default updateChecked(SidebarView);
