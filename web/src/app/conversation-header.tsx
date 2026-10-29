import type { ComponentChildren } from "preact";
import type { AppModal, Session } from "../shared/types.ts";
import { Button, IconButton } from "../shared/ui.tsx";
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
import { useCompact, usePhone } from "../shared/layout.ts";
import { useHostContext } from "../state/host-context.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { isBrowsing } from "./chat-page.tsx";

// The page's title and what acts on the open conversation: the browser, its
// files, Settings on a small screen and the conversation's menu. A phone
// keeps the title and the menu, which takes the rest; the browser stays
// while the agent uses it.
export default function ConversationHeader({
  page,
  selected,
  session,
  parent,
  opening,
  booting,
  placeName,
  browserAvailable,
  terminal,
  filesOpen,
  setFilesOpen,
  openDrawer,
  choose,
  open,
  settings,
  menu,
}: {
  page: "chat" | "library" | "scheduled";
  selected: string;
  session?: Session;
  // The conversation that started this one, when listed.
  parent?: Session;
  opening: boolean;
  booting: boolean;
  placeName: (path?: string) => string;
  browserAvailable: boolean;
  // Present while the host offers terminals.
  terminal?: { open: boolean; toggle: () => void };
  filesOpen: boolean;
  setFilesOpen: (open: boolean) => void;
  openDrawer: () => void;
  choose: (id: string) => void;
  open: (modal: AppModal) => void;
  settings: () => void;
  menu: (
    item: Session,
    withDetail?: boolean,
    extra?: ComponentChildren,
  ) => ComponentChildren;
}) {
  const { store, setModal } = useHostContext();
  const compact = useCompact();
  const phone = usePhone();
  // The browser icon shows when this conversation's agent is using it.
  const browsing = useSnapshots(store, (all) => isBrowsing(all[selected]));
  const openBrowser = () => setModal({ type: "browser" });
  // On a phone the conversation's menu holds the header's other actions;
  // without a conversation there is no menu, and Settings keeps its button.
  const inMenu = phone && page === "chat" && !!(session || opening);
  const titleOf = (item: Session) =>
    item.kind === "panel"
      ? `Lead · ${placeName(item.cwd)}`
      : item.title || "Conversation";
  const phoneItems = inMenu && (
    <>
      <MenuItem disabled={!session} onClick={() => setFilesOpen(!filesOpen)}>
        Files
      </MenuItem>
      {browserAvailable && (
        <MenuItem disabled={booting} onClick={openBrowser}>
          Open browser
        </MenuItem>
      )}
      {terminal && (
        <MenuItem onClick={terminal.toggle}>
          {terminal.open ? "Hide terminal" : "Terminal"}
        </MenuItem>
      )}
      {session?.kind === "panel" && (
        <MenuItem onClick={() => open({ type: "instructions" })}>
          Edit instructions
        </MenuItem>
      )}
      <MenuItem onClick={settings}>Settings</MenuItem>
    </>
  );
  return (
    <header class="conversation-head">
      {compact && (
        <IconButton label="Open conversations" onClick={openDrawer}>
          <Menu />
        </IconButton>
      )}
      <div>
        <h1 title={session?.cwd}>
          {page === "library" ? (
            "Library"
          ) : page === "scheduled" ? (
            "Scheduled"
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
        {/* An agent leads back to the conversation that started it. */}
        {page === "chat" && parent && (
          <p class="panel-subtitle small">
            <Button variant="quiet" onClick={() => choose(parent.id)}>
              ‹ {titleOf(parent)}
            </Button>
          </p>
        )}
        {!phone && page === "chat" && session?.kind === "panel" && (
          <PanelHelp editInstructions={() => open({ type: "instructions" })} />
        )}
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
      {/* Held in place while the session loads, inert. */}
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
          menu(session, true, phoneItems)
        ) : opening ? (
          // Inert until the session is known.
          <IconButton label="Conversation menu" disabled>
            <Ellipsis />
          </IconButton>
        ) : null)}
    </header>
  );
}

// What a panel is, in one line under its title, with the way to
// change what it reads.
function PanelHelp({ editInstructions }: { editInstructions: () => void }) {
  return (
    <p class="panel-subtitle small">
      <span>
        Plans this folder&rsquo;s work and starts agents (↳) for it; never edits
        files itself.
      </span>
      <Button variant="quiet" onClick={editInstructions}>
        Edit instructions
      </Button>
    </p>
  );
}
