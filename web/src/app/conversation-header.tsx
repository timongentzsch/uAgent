import "./conversation-header.css";
import type { ComponentChildren } from "preact";
import type { Outgoing, Session } from "../shared/types.ts";
import { Button, IconButton } from "../shared/ui.tsx";
import { SessionSummary } from "../shared/session-summary.tsx";
import { queuedGuidance } from "../shared/message-view.ts";
import { MenuItem } from "../shared/menu.tsx";
import {
  Ellipsis,
  FolderOpen,
  Globe2,
  Menu,
  Settings,
  Terminal,
} from "lucide-preact";
import { StatusLed } from "../shared/connection-status.tsx";
import { useCompact, useNarrowColumn } from "../shared/layout.ts";
import { useHosts, useMultipleHosts } from "../state/sources.ts";
import { useHostContext } from "../state/host-context.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { isBrowsing } from "./chat-page.tsx";
import type { Page } from "./app.tsx";

function Totals({
  session,
  outgoing,
}: {
  session: Session;
  outgoing: Outgoing[];
}) {
  const { store, setModal } = useHostContext();
  const state = useSnapshots(store, (all) => all[session.id]?.state);
  const queued = outgoing.some(
    (item) => item.session_id === session.id && queuedGuidance(item),
  );
  return (
    <p class="head-subtitle head-totals small">
      <SessionSummary
        state={state}
        queued={queued ? 0 : session.guidance}
        open={() => setModal({ type: "statistics", session_id: session.id })}
      />
    </p>
  );
}

export default function ConversationHeader({
  page,
  selected,
  session,
  parent,
  opening,
  booting,
  browserAvailable,
  terminal,
  filesOpen,
  setFilesOpen,
  outgoing,
  openDrawer,
  choose,
  settings,
  menu,
}: {
  page: Page;
  selected: string;
  session?: Session;
  parent?: Session;
  opening: boolean;
  booting: boolean;
  browserAvailable: boolean;
  terminal?: { open: boolean; toggle: () => void };
  filesOpen: boolean;
  setFilesOpen: (open: boolean) => void;
  outgoing: Outgoing[];
  openDrawer: () => void;
  choose: (id: string) => void;
  settings: () => void;
  menu: (
    item: Session,
    withDetail?: boolean,
    extra?: { top?: ComponentChildren; more?: ComponentChildren },
  ) => ComponentChildren;
}) {
  const { store, setModal } = useHostContext();
  const hosts = useHosts();
  const multipleHosts = useMultipleHosts();
  const executionHost =
    hosts.sources.find((source) => source.id === (session?.host || ""))?.name ||
    "Local";
  const compact = useCompact();
  const narrow = useNarrowColumn();
  const browsing = useSnapshots(store, (all) => isBrowsing(all[selected]));
  const openBrowser = () => setModal({ type: "browser" });
  // A narrow column's conversation menu holds the header's other actions.
  const inMenu = narrow && page === "chat" && !!(session || opening);
  const titleOf = (item: Session) => item.title || "Conversation";
  const phoneItems = inMenu && {
    top: (
      <>
        <MenuItem disabled={!session} onClick={() => setFilesOpen(!filesOpen)}>
          Files
        </MenuItem>
        {terminal && (
          <MenuItem onClick={terminal.toggle}>
            {terminal.open ? "Hide terminal" : "Terminal"}
          </MenuItem>
        )}
      </>
    ),
    more: browserAvailable && (
      <MenuItem disabled={booting} onClick={openBrowser}>
        Open browser
      </MenuItem>
    ),
  };
  return (
    <header class="conversation-head">
      {compact && (
        <IconButton label="Open conversations" onClick={openDrawer}>
          <Menu />
        </IconButton>
      )}
      <div>
        <div class="head-identity">
          <h1
            title={
              session
                ? [titleOf(session), session.cwd].filter(Boolean).join("\n")
                : undefined
            }
          >
            {page === "library" ? (
              "Library"
            ) : page === "scheduled" ? (
              "Scheduled"
            ) : page === "folder" ? (
              "Overview"
            ) : page === "terminal" ? (
              "Terminal"
            ) : session ? (
              titleOf(session)
            ) : opening ? (
              <span class="text-skeleton" aria-hidden="true">
                Loading conversation
              </span>
            ) : (
              "µAgent"
            )}
          </h1>
          {multipleHosts && page === "chat" && session && (
            <span
              class="execution-host small muted truncate"
              title="Execution host"
            >
              {executionHost}
            </span>
          )}
        </div>
        {page === "chat" && parent && (
          <p class="head-subtitle small">
            <Button variant="quiet" onClick={() => choose(parent.id)}>
              <span class="truncate">‹ {titleOf(parent)}</span>
            </Button>
          </p>
        )}
        {inMenu && session && <Totals session={session} outgoing={outgoing} />}
      </div>
      {browserAvailable && (!inMenu || browsing) && (
        <IconButton
          label={browsing ? "Open browser, agent working" : "Open browser"}
          class="browser-toggle"
          disabled={booting}
          onClick={openBrowser}
        >
          <Globe2 />
          {browsing && <StatusLed state="running" />}
        </IconButton>
      )}
      {terminal && !inMenu && (
        <IconButton
          label="Terminal"
          aria-pressed={terminal.open}
          onClick={terminal.toggle}
        >
          <Terminal />
        </IconButton>
      )}
      {!inMenu && page === "chat" && (session || opening) && (
        <IconButton
          label="Files"
          disabled={!session}
          aria-pressed={filesOpen}
          onClick={() => setFilesOpen(!filesOpen)}
        >
          <FolderOpen />
        </IconButton>
      )}
      {compact && !inMenu && (
        <div>
          <IconButton label="Settings" onClick={settings}>
            <Settings />
          </IconButton>
        </div>
      )}
      {page === "chat" &&
        (session ? (
          menu(session, true, phoneItems || undefined)
        ) : opening ? (
          <IconButton label="Conversation menu" disabled>
            <Ellipsis />
          </IconButton>
        ) : null)}
    </header>
  );
}
