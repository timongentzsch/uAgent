import { filingTag, unfiled } from "../../shared/filing.ts";
import { useHosts, type Source } from "../../state/sources.ts";
import {
  Fold,
  SharedShortcuts,
  OtherPlaces,
  HostList,
  unpinned,
} from "./hosts.tsx";
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
import FolderLabel, {
  folderName,
  ownName,
} from "../../shared/folder-label.tsx";
import { folderOf } from "../../shared/display.ts";
import SessionName from "../../shared/session-name.tsx";
import { Menu, MenuItem, useLongPress } from "../../shared/menu.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import { plural } from "../../shared/quantities.ts";
import { statusOf, waiting, type SessionState } from "../../state/attention.ts";
import { collapsed, folded, opened, placeKey, useFolding } from "./folding.ts";
import { DAYS, dayOf } from "./days.ts";
import { PinItem } from "./pins.tsx";
import type { SelectionApi } from "../../shared/selection.ts";
import { useLatest } from "../../shared/use-latest.ts";
import { useShortcuts } from "../../shared/shortcuts.ts";
import { useStored } from "../../shared/storage.ts";
import { devicePrefs } from "../../shared/device-prefs.ts";
import { RowCheck, SelectionBar } from "../../shared/selection-bar.tsx";
import "./sidebar.css";

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

const STATES: Partial<
  Record<SessionState, { label: string; Icon?: typeof CircleAlert }>
> = {
  waiting: { label: "Needs your input", Icon: CircleAlert },
  failed: { label: "Failed", Icon: TriangleAlert },
  working: { label: "Working" },
};

// A conversation and its agents at work; a finished agent leaves the list.
type Entry = { item: Session; agents: Entry[] };

// Places newest first. An agent whose parent is not listed is an ordinary
// row; a workspace is a place whether or not anything is in it yet. What
// waits on a decision is listed apart, under Needs you.
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
  const needs = new Set(waiting(sessions).map((item) => item.id));
  const entry = (item: Session): Entry => ({
    item,
    agents: sorted
      .filter(
        (agent) =>
          agent.parent === item.id &&
          !needs.has(agent.id) &&
          statusOf(agent, online) !== "idle",
      )
      .map(entry),
  });
  for (const item of sorted)
    if ((!item.parent || !listed.has(item.parent)) && !needs.has(item.id))
      place(folderOf(item)).push(entry(item));
  for (const space of spaces) place(space.path);
  return {
    groups,
    needs: sorted.filter((item) => needs.has(item.id)).map(entry),
  };
}

const rowsOf = ({ item, agents }: Entry): Session[] => [
  item,
  ...agents.flatMap(rowsOf),
];

const newest = (a: { item: Session }, b: { item: Session }) =>
  (b.item.updated || 0) - (a.item.updated || 0);

// Pinned rows in pin order, then the other places by kind, workspaces
// first. A pinned conversation or folder is listed once, among the pinned.
type Place = [string, Entry[]];
type Pinned = { entry: Entry } | { place: Place } | { terminal: TerminalInfo };

function arrange(
  sessions: Session[],
  spaces: Space[] = [],
  online = true,
  pins: Pin[] = [],
  terminals: TerminalInfo[] = [],
) {
  const { groups: grouped, needs } = groupSessions(sessions, spaces, online);
  const groups = [...grouped];
  const space = (path: string) => spaces.some((item) => item.path === path);
  const pinned = (kind: Pin["kind"], id: string) =>
    pins.some((pin) => pin.kind === kind && pin.id === id);
  const entries = [...needs, ...groups.flatMap(([, entries]) => entries)];
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
    needs: needs.filter((entry) => !pinned("session", entry.item.id)),
    recent: rest.flatMap(([, entries]) => entries).sort(newest),
    kinds: (
      [
        ["Workspaces", rest.filter(([path]) => space(path))],
        ["Folders", rest.filter(([path]) => !space(path))],
      ] as const
    ).filter(([, shown]) => shown.length),
  };
}

type View = "recent" | "places";

// A place shows its last week, at least its three newest and the open
// conversation; the rest wait behind "N older".
function placeRows(entries: Entry[], key: string, selected: string) {
  const old = entries.filter(
    (entry, index) =>
      index > 2 &&
      dayOf(entry.item.updated) > 2 &&
      !rowsOf(entry).some((item) => item.id === selected),
  );
  const fresh = entries.filter((entry) => !old.includes(entry));
  return { fresh, old, open: opened.has(key) };
}
const olderKey = (host: string, path: string) =>
  `$older:${placeKey(host, path)}`;

// The ids of the rows the list shows, in order: what a Shift run, ⌘A and
// Alt+↑/↓ follow. In Places a workspace is a row of its own.
function orderOf(
  { top, needs, recent, kinds }: ReturnType<typeof arrange>,
  view: View,
  host: string,
  single: boolean,
  selected: string,
  spaces: Space[] = [],
) {
  const rows = (entries: Entry[]) =>
    entries.flatMap(rowsOf).map((item) => item.id);
  const place = ([path, entries]: Place) => {
    const { fresh, old, open } = placeRows(
      entries,
      olderKey(host, path),
      selected,
    );
    return [
      ...(view === "places" && spaces.some((space) => space.path === path)
        ? [path]
        : []),
      ...(collapsed.has(placeKey(host, path))
        ? []
        : rows(open ? [...fresh, ...old] : fresh)),
    ];
  };
  return [
    ...(folded("$pinned") ? [] : top).flatMap((pinned) =>
      "entry" in pinned
        ? rows([pinned.entry])
        : "place" in pinned
          ? place(pinned.place)
          : [],
    ),
    ...rows(needs),
    ...(view === "recent"
      ? DAYS.flatMap((day, index) =>
          folded(`$day:${day}`, index === DAYS.length - 1)
            ? []
            : rows(
                recent.filter((entry) => dayOf(entry.item.updated) === index),
              ),
        )
      : single || !folded(`$host:${host}`)
        ? kinds.flatMap(([, places]) => places.flatMap(place))
        : []),
  ];
}

// The rows in the order the list shows them: what Alt+↑/↓ steps through.
export const sessionOrder = (
  sessions: Session[],
  spaces: Space[] | undefined,
  online: boolean,
  pins: Pin[],
  host: string,
  single: boolean,
  selected: string,
) =>
  orderOf(
    arrange(sessions, spaces, online, pins),
    devicePrefs.list.read(),
    host,
    single,
    selected,
    spaces,
  ).flatMap((id) => sessions.find((item) => item.id === id) || []);

// What Recent shows on a row's right: the workspace's name or the folder's
// own, after its host's where that is another one.
const tagOf = (item: Session, spaces: Space[] = [], host = "") => {
  const path = folderOf(item);
  const place =
    spaces.find((space) => space.path === path)?.name || ownName(path);
  return host ? `${host} · ${place}` : place;
};

type SessionRowProps = {
  item: Session;
  selected: boolean;
  unread: boolean;
  online: boolean;
  // Its place, shown instead of its time.
  tag?: string;
  choose: (id: string) => void;
  // Absent on another host's row.
  menu?: (session: Session) => ComponentChildren;
  picked?: boolean;
  selecting?: boolean;
  pick?: (id: string, event: MouseEvent) => boolean;
  toggle?: (id: string) => void;
};

function SessionRow({
  item,
  selected,
  unread,
  online,
  tag,
  choose,
  menu,
  picked = false,
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
          toggle={() => toggle?.(item.id)}
        />
      )}
      <ListRow
        class="session"
        onClick={(event) => pick?.(item.id, event) || choose(item.id)}
        aria-current={selected ? "page" : undefined}
        data-unread={unread || undefined}
        title={
          <>
            <span
              class={`session-state ${state}`}
              role={shown && "img"}
              aria-label={shown?.label}
            >
              {shown &&
                (shown.Icon ? (
                  <shown.Icon aria-hidden />
                ) : (
                  <StatusLed state="running" />
                ))}
            </span>
            <SessionName item={item} />
            {unread && <span role="img" aria-label="Unread messages" />}
          </>
        }
      >
        {tag !== undefined ? (
          <span class="session-tag truncate">{tag}</span>
        ) : item.updated ? (
          <span class="session-time">
            <Time value={item.updated} brief />
          </span>
        ) : null}
      </ListRow>
      {menu?.(item)}
    </div>
  );
}

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
          <Time value={terminal.started} brief />
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
  spaces: Space[];
  pins: Pin[];
  spaceMenu: (space: Space, conversations: number) => ComponentChildren;
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
  create: (place: "workspace" | "folder") => void;
  start: (cwd: string) => void;
  home: () => void;
  overview: (cwd: string) => void;
  shownPlace: string;
  terminals?: TerminalInfo[] | null;
  shownTerminal?: string;
  terminalMenu: (terminal: TerminalInfo) => ComponentChildren;
  showTerminal: (terminal: TerminalInfo) => void;
  newTerminal?: (folder: string) => void;
  search: () => void;
};

function SidebarView({
  loading,
  sessions,
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
  home,
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
  const host = hosts.active;
  const several = hosts.sources.length > 1;
  const [view, setView] = useStored(devicePrefs.list);
  useFolding();
  const rows = useRef<HTMLElement>(null);
  useLongPress(rows, ".session-row, .place-head");
  const drawing = loading && !sessions.length;
  const all = drawing ? SAMPLE : sessions;
  // Not regrouped for a selection, an unread mark or a fold.
  const arranged = useMemo(
    () => arrange(all, drawing ? [] : spaces, online, pins, terminals || []),
    [all, drawing, spaces, online, pins, terminals],
  );
  const { top, needs, recent, kinds } = arranged;
  const order = orderOf(arranged, view, host, !several, selected, spaces);
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
  const counts = useMemo(() => {
    const counts = new Map<string, number>();
    for (const item of all)
      counts.set(folderOf(item), (counts.get(folderOf(item)) || 0) + 1);
    return counts;
  }, [all]);
  const row = (item: Session, filing?: string) => (
    <SessionEntry
      key={item.id}
      item={item}
      selected={item.id === selected}
      unread={unread.has(item.id)}
      online={online}
      tag={filing ?? (view === "recent" ? tagOf(item, spaces) : undefined)}
      choose={choose}
      menu={menu}
      picked={selection.ids.has(item.id)}
      selecting={selection.selecting}
      pick={pick}
      toggle={toggle}
    />
  );
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
  // Another host's row opens that host at the conversation.
  const remoteRow = (source: Source, item: Session) => (
    <SessionEntry
      key={`${source.id}:${item.id}`}
      item={item}
      selected={false}
      unread={false}
      online={source.phase === "connected"}
      tag={tagOf(item, source.state.catalogue.spaces, source.name)}
      choose={(id) => hosts.open(source.id, id)}
    />
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
  // A folder goes by its own name, with its parent's only where two share it.
  const folderNames = [
    ...top.flatMap((pinned) => ("place" in pinned ? [pinned.place] : [])),
    ...kinds.flatMap(([, places]) => places),
  ]
    .filter(([path]) => !kept.has(path))
    .map(([path]) => ownName(path));
  const place = ([cwd, entries]: Place) => {
    const space = kept.get(cwd);
    const name = space?.name || folderName(cwd);
    const shut = collapsed.has(placeKey(host, cwd));
    const conversations = entries.flatMap(rowsOf);
    const every = conversations.every((item) => selection.ids.has(item.id));
    const some = conversations.some((item) => selection.ids.has(item.id));
    const checks = selection.selecting && view === "places";
    const shells = (terminals || []).filter(
      (item) =>
        item.folder === cwd &&
        !pins.some((pin) => pin.kind === "terminal" && pin.id === item.id),
    );
    const holdsOpen = conversations.some((item) => item.id === selected);
    const attention = conversations.filter((item) =>
      ["waiting", "failed"].includes(statusOf(item, online)),
    ).length;
    const older = olderKey(host, cwd);
    const { fresh, old, open } = placeRows(entries, older, selected);
    const own = ownName(cwd);
    return (
      <section key={cwd}>
        <div
          class="place-head entry"
          data-picked={selection.ids.has(cwd) || undefined}
        >
          {checks && space && (
            <RowCheck
              picked={selection.ids.has(cwd)}
              label={`Select the workspace ${name}`}
              toggle={() => toggle(cwd)}
            />
          )}
          {checks && !space && conversations.length > 0 && (
            <RowCheck
              picked={every}
              mixed={!every && some}
              label={`Select the conversations of ${name}`}
              toggle={() =>
                selection.set(
                  conversations.map((item) => item.id),
                  !every,
                )
              }
            />
          )}
          <IconButton
            label={`Conversations of ${name}`}
            aria-expanded={!shut}
            aria-current={shut && holdsOpen ? "true" : undefined}
            onClick={(event) => {
              // Focus stays on the header, not in the rows it takes away.
              event.currentTarget.focus();
              collapsed.set(placeKey(host, cwd), !shut);
            }}
          >
            {shut ? <ChevronRight /> : <ChevronDown />}
          </IconButton>
          <h2>
            <Button
              variant="quiet"
              title={`Overview of ${name}`}
              disabled={drawing}
              aria-current={shownPlace === cwd ? "page" : undefined}
              onClick={(event) =>
                (space && view === "places" && pick(cwd, event)) ||
                overview(cwd)
              }
            >
              <FolderLabel path={cwd} space={!!space}>
                {space?.name ||
                  (folderNames.filter((other) => other === own).length > 1
                    ? undefined
                    : own)}
              </FolderLabel>
              {conversations.length > 0 && (
                <span
                  class="place-count"
                  aria-label={plural(conversations.length, "conversation")}
                >
                  {conversations.length}
                </span>
              )}
              {shut && attention > 0 && (
                <span
                  class="status-led unread attention"
                  role="img"
                  aria-label={`${attention} need attention`}
                />
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
        {!shut && (
          <div class="place-body">
            {shells.map(terminalRow)}
            {fresh.map(conversation)}
            {old.length > 0 && (
              <Button
                variant="quiet"
                class="section-fold caption"
                aria-expanded={open}
                onClick={() => opened.set(older, !open)}
              >
                {open ? <ChevronDown /> : <ChevronRight />}
                {old.length} older
              </Button>
            )}
            {open && old.map(conversation)}
          </div>
        )}
      </section>
    );
  };
  // Only what has a suggestion: the rest of Scratch is under its workspace.
  const toFile = drawing
    ? []
    : unfiled(all, spaces).filter((item) => filingTag(item, spaces));
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
  // Recent: every host's conversations, newest first, by day.
  const latestRows =
    view === "recent"
      ? [
          ...recent.map((entry) => ({ entry, source: undefined })),
          ...hosts.sources
            .filter((source) => source.id !== host)
            .flatMap((source) =>
              unpinned(source).map((item) => ({
                entry: { item, agents: [] },
                source,
              })),
            ),
        ].sort((a, b) => newest(a.entry, b.entry))
      : [];
  const empty = !activePins.length &&
    !needs.length &&
    !(view === "recent" ? latestRows.length : kinds.length) && (
      <EmptyState key="empty">
        No conversations yet. New starts one in a workspace or folder on this
        host.
      </EmptyState>
    );
  const placesOf = (source: Source) => {
    if (source.id !== host)
      return <OtherPlaces key={source.id} source={source} />;
    const shown = [
      ...kinds.map(([kind, shown]) => group(kind, shown.map(place))),
      empty,
    ];
    return several ? (
      <Fold key={source.id} name={`$host:${host}`} label={source.name}>
        {shown}
      </Fold>
    ) : (
      <Fragment key={source.id}>{shown}</Fragment>
    );
  };
  const list = [
    <SharedShortcuts key="pinned" activePins={activePins} />,
    needs.length > 0 && group("Needs you", needs.map(conversation)),
    toFile.length > 0 &&
      group(
        "To file",
        toFile.map((item) => row(item, filingTag(item, spaces) ?? "")),
      ),
    ...(view === "recent"
      ? [
          ...DAYS.map((day, index) => {
            const rows = latestRows.filter(
              ({ entry }) => dayOf(entry.item.updated) === index,
            );
            return (
              rows.length > 0 && (
                <Fold
                  key={day}
                  name={`$day:${day}`}
                  label={day}
                  closed={index === DAYS.length - 1}
                >
                  {rows.map(({ entry, source }) =>
                    source
                      ? remoteRow(source, entry.item)
                      : conversation(entry),
                  )}
                </Fold>
              )
            );
          }),
          empty,
        ]
      : hosts.sources.map(placesOf)),
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
          <MenuItem disabled={!online} onClick={home}>
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
      <div class="sidebar-sections segmented" role="group" aria-label="List">
        {(
          [
            ["recent", "Recent"],
            ["places", "Places"],
          ] as const
        ).map(([name, label]) => (
          <Button
            key={name}
            variant="secondary"
            aria-pressed={view === name}
            onClick={() => setView(name)}
          >
            {label}
          </Button>
        ))}
      </div>
      <nav
        ref={rows}
        aria-label="Conversations"
        data-selecting={selection.selecting || undefined}
      >
        <Placeholder label="Loading conversations…" when={drawing}>
          {list}
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
