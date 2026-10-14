import { ConnectionStatus } from "../../shared/connection-status.tsx";
import { Component, type ComponentChildren } from "preact";
import type { Session, Space, AppModal } from "../../shared/types.ts";
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
import { ActivityStatus, active } from "../../shared/activity-status.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import {
  needsYou,
  statusOf,
  type SessionState,
} from "../../state/attention.ts";
import { SheetButton } from "../../shared/sheet.tsx";
import WaitingList from "../../shared/escalations.tsx";
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

// What can be done to a space, which is µAgent's own to rename and remove.
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
    <Menu label={`Menu for the space ${space.name}`}>
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

// A row that waits on you or failed shows that as a shape in place of the
// LED; a working row keeps the LED, which breathes. The words beside it name
// the state.
const STATE_ICONS: Partial<Record<SessionState, typeof Inbox>> = {
  waiting: CircleAlert,
  failed: TriangleAlert,
};

type Place = { panel?: Session; threads: Session[]; others: Session[] };

// The list's places in its order, newest first: each one's panel, what
// the panel started (threads and the members of its chat), then its
// conversations. A space is a place whether or not anything is in it yet.
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

// The places under the word for their kind, spaces first: one is µAgent's
// to keep and remove, the other a directory of yours.
function placesByKind(sessions: Session[], spaces: Space[] = [], search = "") {
  const places = [...groupSessions(sessions, spaces, search)];
  const space = (path: string) => spaces.some((item) => item.path === path);
  return (
    [
      ["Spaces", places.filter(([path]) => space(path))],
      ["Folders", places.filter(([path]) => !space(path))],
    ] as const
  ).filter(([, shown]) => shown.length);
}

// The rows in the order the list shows them: what Alt+↑/↓ steps through.
export const sessionOrder = (sessions: Session[], spaces?: Space[]) =>
  placesByKind(sessions, spaces).flatMap(([, places]) =>
    places.flatMap(([, group]) => [
      ...(group.panel ? [group.panel] : []),
      ...group.threads,
      ...group.others,
    ]),
  );

type SessionRowProps = {
  item: Session;
  selected: boolean;
  unread: boolean;
  online: boolean;
  choose: (id: string) => void;
  menu: (session: Session) => ComponentChildren;
  // A panel's row says how many members its chat has.
  members?: number;
};

// One conversation in the list: its title, activity and last update.
function SessionRowView({
  item,
  selected,
  unread,
  online,
  choose,
  menu,
  members = 0,
}: SessionRowProps) {
  const state = statusOf(item, online);
  const Icon = STATE_ICONS[state];
  return (
    <div class="session-row">
      <ListRow
        class="session"
        onClick={() => choose(item.id)}
        aria-current={selected ? "page" : undefined}
        title={
          <>
            <SessionName item={item} />
            {members > 0 && ` · ${members} member${members > 1 ? "s" : ""}`}
          </>
        }
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
  spaces: Space[];
  // What can be done to a space: rename it, delete it.
  spaceMenu: (space: Space, conversations: number) => ComponentChildren;
  selected: string;
  unread: Set<string>;
  choose: (id: string) => void;
  menu: (session: Session) => ComponentChildren;
  settings: () => void;
  create: () => void;
  openPanel: (cwd: string) => void;
  palette: () => void;
};

function SidebarView({
  loading,
  sessions,
  spaces,
  spaceMenu,
  selected,
  unread,
  choose,
  menu,
  settings,
  create,
  openPanel,
  page,
  navigate,
  scheduledUnread,
  palette,
}: SidebarProps) {
  const { online, connection, refresh } = useHostContext();
  const [search, setSearch] = useState("");
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
  const count = drawing ? 0 : needsYou(all);
  const kept = new Map(spaces.map((space) => [space.path, space]));
  const place = ([cwd, group]: [string, Place]) => {
    const space = kept.get(cwd);
    return (
      <section key={cwd}>
        <h2>
          <FolderLabel path={cwd} space={!!space}>
            {space?.name}
          </FolderLabel>
          {space &&
            spaceMenu(
              space,
              all.filter((item) => folderOf(item) === cwd).length,
            )}
        </h2>
        {group.panel ? (
          <SessionRow
            key={group.panel.id}
            item={group.panel}
            selected={group.panel.id === selected}
            unread={unread.has(group.panel.id)}
            online={online}
            choose={choose}
            menu={menu}
            members={group.threads.filter((item) => item.member).length}
          />
        ) : (
          !drawing &&
          !search && (
            <Button
              variant="quiet"
              class="open-panel"
              disabled={!online}
              onClick={() => openPanel(cwd)}
            >
              <MessagesSquare />
              Open panel
            </Button>
          )
        )}
        {group.threads.length > 0 && (
          <div
            class="threads"
            role="group"
            aria-label={`Started by the panel of ${space?.name || folderName(cwd)}`}
          >
            {group.threads.map(row)}
          </div>
        )}
        {group.others.map(row)}
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
                : "No conversations yet. New conversation starts one in a folder on your host."}
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
