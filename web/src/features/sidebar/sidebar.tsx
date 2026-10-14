import { ConnectionStatus } from "../../shared/connection-status.tsx";
import { Component, type ComponentChildren } from "preact";
import type { Session, Space, AppModal } from "../../shared/types.ts";
import { useState } from "preact/hooks";
import {
  Check,
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
import { command, manage } from "../../state/api.ts";
import { useHostContext } from "../../state/host-context.ts";
import {
  Mark,
  Input,
  Time,
  Button,
  IconButton,
  Placeholder,
  EmptyState,
} from "../../shared/ui.tsx";
import FolderLabel, { folderName } from "../../shared/folder-label.tsx";
import { folderOf } from "../../shared/display.ts";
import SessionName from "../../shared/session-name.tsx";
import { Menu, MenuItem, MenuSub } from "../../shared/menu.tsx";
import { levelLabel } from "../../shared/verbosity.ts";
import { StatusLed } from "../../shared/connection-status.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import { plural } from "../../shared/quantities.ts";
import { statusOf, type SessionState } from "../../state/attention.ts";
import { SheetButton } from "../../shared/sheet.tsx";
import WaitingList from "../../shared/escalations.tsx";
import { collapsed, panelsOpen, useFolding } from "./folding.ts";
import "./sidebar.css";
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

// What can be done to a workspace, which is µAgent's own to rename and remove.
export function SpaceMenu({
  space,
  conversations,
  open,
}: {
  space: Space;
  conversations: number;
  open: (modal: AppModal) => void;
}) {
  const { online } = useHostContext();
  return (
    <Menu label={`Menu for the workspace ${space.name}`}>
      <MenuItem
        disabled={!online}
        onClick={() => open({ type: "rename", space })}
      >
        Rename
      </MenuItem>
      <MenuItem
        disabled={!online}
        onClick={() => open({ type: "delete", space, conversations })}
      >
        Delete
      </MenuItem>
    </Menu>
  );
}

export function ConversationMenu({
  item,
  fork,
  notify,
  open,
  detail,
}: {
  item: Session;
  fork: (item: Session) => void;
  // Says what an action did, where the conversation shows its notices.
  notify: (text: string) => void;
  open: (modal: AppModal) => void;
  // How much of the agent's work every transcript shows, and changing it:
  // one level for all conversations, offered where a transcript is read.
  detail?: {
    levels: string[];
    level: string;
    disabled: boolean;
    change: (level: string) => void;
  };
}) {
  const { online, load: loadSnapshot, report } = useHostContext();
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
      {detail && detail.levels.length > 0 && (
        <MenuSub label="Detail" value={levelLabel(detail.level)}>
          {detail.levels.map((level) => (
            <MenuItem
              key={level}
              role="menuitemradio"
              aria-checked={level === detail.level}
              disabled={detail.disabled}
              onClick={() => detail.change(level)}
            >
              {levelLabel(level)}
              {level === detail.level && <Check />}
            </MenuItem>
          ))}
          <p class="menu-note">For all conversations.</p>
        </MenuSub>
      )}
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
          Close conversation
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

// What a row says of its conversation, at its right edge: it waits on you,
// it failed, or it works (the LED breathes). An idle one says nothing.
const STATES: Partial<
  Record<SessionState, { label: string; Icon?: typeof Inbox }>
> = {
  waiting: { label: "Needs your input", Icon: CircleAlert },
  failed: { label: "Failed", Icon: TriangleAlert },
  working: { label: "Working" },
};

type Place = { panel?: Session; threads: Session[]; others: Session[] };

// The list's places in its order, newest first: each one's panel, what
// the panel started (threads and the members of its chat), then its
// conversations. A workspace is a place whether or not anything is in it yet.
function groupSessions(sessions: Session[], spaces: Space[] = [], search = "") {
  const groups = new Map<string, Place>();
  const place = (path: string) => {
    if (!groups.has(path)) groups.set(path, { threads: [], others: [] });
    return groups.get(path)!;
  };
  const names = new Map(spaces.map((space) => [space.path, space.name]));
  const matches = (text: string) =>
    text.toLowerCase().includes(search.toLowerCase());
  for (const item of [...sessions].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  )) {
    const path = folderOf(item);
    // A panel is listed with its place, whatever the search shows of it.
    if (item.kind === "panel") {
      if (!search || matches(`${names.get(path) || ""} ${path}`))
        place(path).panel = item;
    } else if (matches(`${item.title} ${names.get(path) || ""} ${item.cwd}`)) {
      (item.kind === "thread" ? place(path).threads : place(path).others).push(
        item,
      );
    }
  }
  for (const space of spaces) if (matches(space.name)) place(space.path);
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

// A panel's members and threads show under it once it is opened, or at once
// when the place has no panel row to open.
const threadsShown = ([path, group]: [string, Place]) =>
  !group.panel || panelsOpen.has(path);

// The rows in the order the list shows them: what Alt+↑/↓ steps through.
// What is folded away is not in it.
export const sessionOrder = (sessions: Session[], spaces?: Space[]) =>
  placesByKind(sessions, spaces).flatMap(([, places]) =>
    places
      .filter(([path]) => !collapsed.has(path))
      .flatMap((place) => [
        ...(place[1].panel ? [place[1].panel] : []),
        ...(threadsShown(place) ? place[1].threads : []),
        ...place[1].others,
      ]),
  );

type SessionRowProps = {
  item: Session;
  selected: boolean;
  // The open conversation is under this row, which is folded.
  holds?: boolean;
  unread: boolean;
  online: boolean;
  choose: (id: string) => void;
  menu: (session: Session) => ComponentChildren;
  // A panel's row says who is in it and what it started.
  note?: string;
};

// One conversation in the list, on one line: its title and its state. The
// time and the menu show while the row is pointed at or focused.
function SessionRowView({
  item,
  selected,
  holds,
  unread,
  online,
  choose,
  menu,
  note,
}: SessionRowProps) {
  const state = statusOf(item, online);
  const shown = STATES[state];
  return (
    <div class="session-row">
      <ListRow
        class="session"
        onClick={() => choose(item.id)}
        aria-current={selected ? "page" : holds ? "true" : undefined}
        title={
          <>
            <SessionName item={item} />
            {note && ` · ${note}`}
          </>
        }
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
  palette: () => void;
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
  palette,
}: SidebarProps) {
  const { online, connection, refresh } = useHostContext();
  const [search, setSearch] = useState("");
  useFolding();
  // Until the list arrives, it draws sample rows in its own layout.
  const drawing = loading && !sessions.length;
  const all = drawing ? SAMPLE : sessions;
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
  const kept = new Map(spaces.map((space) => [space.path, space]));
  const place = ([cwd, group]: [string, Place]) => {
    const space = kept.get(cwd);
    const name = space?.name || folderName(cwd);
    // A search shows what it found, whatever is folded.
    const folded = !search && collapsed.has(cwd);
    const open = !!search || panelsOpen.has(cwd);
    const members = group.threads.filter((item) => item.member).length;
    const note = [
      members && plural(members, "member"),
      group.threads.length > members &&
        plural(group.threads.length - members, "thread"),
    ]
      .filter(Boolean)
      .join(" · ");
    const conversations = [...group.threads, ...group.others];
    const holdsOpen = [group.panel, ...conversations].some(
      (item) => item?.id === selected,
    );
    const attention = [group.panel, ...conversations].filter(
      (item) => item && ["waiting", "failed"].includes(statusOf(item, online)),
    ).length;
    return (
      <section key={cwd}>
        <div class="place-head">
          <h2>
            <Button
              variant="quiet"
              aria-expanded={!folded}
              aria-current={folded && holdsOpen ? "true" : undefined}
              onClick={() => collapsed.set(cwd, !folded)}
            >
              {folded ? <ChevronRight /> : <ChevronDown />}
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
          {space &&
            spaceMenu(
              space,
              all.filter((item) => folderOf(item) === cwd).length,
            )}
        </div>
        {!folded && (
          <div class="place-body">
            {group.panel ? (
              <SessionRow
                key={group.panel.id}
                item={group.panel}
                selected={group.panel.id === selected}
                holds={
                  !open && group.threads.some((item) => item.id === selected)
                }
                unread={unread.has(group.panel.id)}
                online={online}
                choose={choose}
                menu={(panel) => (
                  <>
                    {group.threads.length > 0 && (
                      <IconButton
                        class="disclosure"
                        label={`Members and threads of ${name}`}
                        aria-expanded={open}
                        onClick={() => panelsOpen.set(cwd, !open)}
                      >
                        {open ? <ChevronDown /> : <ChevronRight />}
                      </IconButton>
                    )}
                    {menu(panel)}
                  </>
                )}
                note={note}
              />
            ) : (
              !drawing &&
              !search && (
                <div class="session-row">
                  <Button
                    variant="quiet"
                    class="open-panel"
                    aria-label={`Open panel of ${name}`}
                    disabled={!online}
                    onClick={() => openPanel(cwd)}
                  >
                    <span>
                      <span class="thread-mark" aria-hidden="true">
                        ◆
                      </span>
                      Panel
                    </span>
                  </Button>
                </div>
              )
            )}
            {group.threads.length > 0 && (!group.panel || open) && (
              <div
                class="threads"
                role="group"
                aria-label={`Members and threads of ${name}`}
              >
                {group.threads.map(row)}
              </div>
            )}
            {group.others.map(row)}
          </div>
        )}
      </section>
    );
  };
  const list = placesByKind(all, drawing ? [] : spaces, search).map(
    ([kind, shown]) => (
      <div key={kind} class="list-group" role="group" aria-label={kind}>
        <p class="list-kind">{kind}</p>
        {shown.map(place)}
      </div>
    ),
  );
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
          value={search}
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
              {search
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
export default class Sidebar extends Component<SidebarProps> {
  shouldComponentUpdate(next: SidebarProps) {
    return changed(this.props, next);
  }
  render(props: SidebarProps) {
    return <SidebarView {...props} />;
  }
}
