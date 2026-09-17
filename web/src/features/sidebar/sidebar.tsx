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
} from "lucide-preact";
import { command } from "../../state/api.ts";
import { Mark } from "../../shared/ui.tsx";
import FolderLabel from "./folder-label.tsx";
import { Menu, MenuItem } from "../../shared/popover.tsx";
import { ActivityStatus, StatusLed, active } from "../chat/activity-status.tsx";
const shortDate = new Intl.DateTimeFormat(undefined, {
  day: "numeric",
  month: "short",
  hour: "2-digit",
  minute: "2-digit",
});
const fullDate = new Intl.DateTimeFormat(undefined, {
  dateStyle: "medium",
  timeStyle: "medium",
});
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

export default function Sidebar({
  sessions,
  selected,
  unread,
  online,
  connecting,
  choose,
  menu,
  refresh,
  settings,
  create,
  page,
  navigate,
  scheduledUnread,
}: {
  page: string;
  navigate: (page: "chat" | "library" | "scheduled") => void;
  scheduledUnread: boolean;
  sessions: Session[];
  selected: string;
  unread: Set<string>;
  online: boolean;
  connecting: boolean;
  choose: (id: string) => void;
  menu: (session: Session) => ComponentChildren;
  refresh: () => void;
  settings: () => void;
  create: () => void;
}) {
  const [search, setSearch] = useState("");
  const groups = new Map<string, Session[]>();
  for (const item of [...sessions]
    .sort((a, b) => (b.updated || 0) - (a.updated || 0))
    .filter((item) =>
      `${item.title} ${item.cwd}`.toLowerCase().includes(search.toLowerCase()),
    )) {
    if (!groups.has(item.cwd || "")) groups.set(item.cwd || "", []);
    groups.get(item.cwd || "")!.push(item);
  }
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
        <button class="quiet with-icon" onClick={create} disabled={!online}>
          <Plus />
          New conversation
        </button>
      </div>
      <div class="sidebar-sections">
        <button
          class="quiet with-icon"
          aria-current={page === "library" ? "page" : undefined}
          onClick={() => navigate("library")}
        >
          <Library />
          Library
        </button>
        <button
          class="quiet with-icon"
          aria-current={page === "scheduled" ? "page" : undefined}
          onClick={() => navigate("scheduled")}
        >
          <CalendarClock />
          Scheduled
          {scheduledUnread && (
            <span class="unread-dot" aria-label="Unread scheduled results" />
          )}
        </button>
      </div>
      <label class="search">
        <span class="sr-only">Find a session</span>
        <input
          type="search"
          value={search}
          onInput={(event) => setSearch(event.currentTarget.value)}
          placeholder="Find a conversation…"
        />
      </label>
      <nav>
        {[...groups].map(([cwd, items]) => (
          <section key={cwd}>
            <h2>
              <FolderLabel path={cwd} />
            </h2>
            {items.map((item) => (
              <div class="session-row" key={item.id}>
                <button
                  class={`session ${item.id === selected ? "selected" : ""}`}
                  onClick={() => choose(item.id)}
                  aria-current={item.id === selected ? "page" : undefined}
                >
                  <span>
                    {item.title || "Untitled conversation"}
                    {unread.has(item.id) && (
                      <span class="unread-dot" aria-label="Unread messages" />
                    )}
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
                    {item.updated ? (
                      <time
                        dateTime={new Date(item.updated).toISOString()}
                        title={fullDate.format(item.updated)}
                      >
                        {shortDate.format(item.updated)}
                      </time>
                    ) : (
                      "New conversation"
                    )}
                    {item.error && (
                      <span title={item.error}> · Needs attention</span>
                    )}
                  </small>
                </button>
                {menu(item)}
              </div>
            ))}
          </section>
        ))}
      </nav>
      <footer>
        <span
          class={`connection ${online ? "connected" : ""}`}
          title={
            online ? "Connected" : connecting ? "Connecting…" : "Disconnected"
          }
        >
          <StatusLed state={online ? "active" : "idle"} />{" "}
          {online ? "Connected" : connecting ? "Connecting…" : "Disconnected"}
        </span>
        <button
          class="quiet icon-button"
          onClick={refresh}
          aria-label="Refresh"
          title="Refresh"
        >
          <RefreshCw />
        </button>
        <button
          class="quiet icon-button"
          onClick={settings}
          aria-label="Settings"
          title="Settings"
        >
          <Settings />
        </button>
      </footer>
    </>
  );
}
