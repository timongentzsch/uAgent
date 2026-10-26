import type { ComponentChildren } from "preact";
import { useState } from "preact/hooks";
import type { AppModal, InspectorTarget, Session } from "../shared/types.ts";
import { Button, IconButton, Modal } from "../shared/ui.tsx";
import { MenuItem } from "../shared/menu.tsx";
import {
  Ellipsis,
  FolderOpen,
  Globe2,
  ListTree,
  Menu,
  Settings,
} from "lucide-preact";
import { StatusLed } from "../shared/connection-status.tsx";
import Board, { PanelHelp } from "../features/panel/board.tsx";
import { waiting as waitingOn } from "../state/attention.ts";
import { useCompact, usePhone } from "../shared/layout.ts";
import { useHostContext } from "../state/host-context.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { isBrowsing } from "./chat-page.tsx";

// The page's title and what acts on the open conversation: the browser, its
// files, a panel's board (a sheet on a phone), Settings on a small screen
// and the conversation's menu. A phone keeps the title, the board and the
// menu, which takes the rest; the browser stays while the agent uses it.
export default function ConversationHeader({
  page,
  selected,
  session,
  opening,
  booting,
  threads,
  placeName,
  browserAvailable,
  filesOpen,
  setFilesOpen,
  openDrawer,
  openPanel,
  open,
  settings,
  menu,
  choose,
  openInspector,
}: {
  page: "chat" | "library" | "scheduled";
  selected: string;
  session?: Session;
  opening: boolean;
  booting: boolean;
  // The open panel's threads.
  threads: Session[];
  placeName: (path?: string) => string;
  browserAvailable: boolean;
  filesOpen: boolean;
  setFilesOpen: (open: boolean) => void;
  openDrawer: () => void;
  openPanel: (cwd: string) => void;
  open: (modal: AppModal) => void;
  settings: () => void;
  menu: (
    item: Session,
    withDetail?: boolean,
    extra?: ComponentChildren,
  ) => ComponentChildren;
  choose: (id: string) => Promise<void>;
  openInspector: (target: InspectorTarget) => void;
}) {
  const { store, setModal } = useHostContext();
  const compact = useCompact();
  const phone = usePhone();
  // On a phone a panel's board slides in from the right on demand
  // instead of taking the top of its chat.
  const [boardOpen, setBoardOpen] = useState(false);
  // The browser icon shows when this conversation's agent is using it.
  const browsing = useSnapshots(store, (all) => isBrowsing(all[selected]));
  // The board's sheet reads the panel's plan and subagents.
  const boardState = useSnapshots(store, (all) => all[selected]?.state);
  const openBrowser = () => setModal({ type: "browser" });
  // On a phone the conversation's menu holds the header's other actions;
  // without a conversation there is no menu, and Settings keeps its button.
  const inMenu = phone && page === "chat" && !!(session || opening);
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
      {session?.kind === "panel" && (
        <MenuItem onClick={() => open({ type: "instructions" })}>
          Edit instructions
        </MenuItem>
      )}
      <MenuItem onClick={settings}>Settings</MenuItem>
    </>
  );
  return (
    <>
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
            ) : session?.kind === "panel" ? (
              `Lead · ${placeName(session.cwd)}`
            ) : session ? (
              session.title || "Your workspace"
            ) : opening ? (
              <span class="text-skeleton" aria-hidden="true">
                Loading conversation
              </span>
            ) : (
              "Your workspace"
            )}
          </h1>
          {/* What a panel started leads back to it. */}
          {page === "chat" && session?.kind === "thread" && (
            <p class="panel-subtitle">
              <Button
                variant="quiet"
                onClick={() => openPanel(session.folder || "")}
              >
                ‹ Lead · {placeName(session.folder)}
              </Button>
            </p>
          )}
          {!phone && page === "chat" && session?.kind === "panel" && (
            <PanelHelp
              editInstructions={() => open({ type: "instructions" })}
            />
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
        {phone &&
          page === "chat" &&
          session?.kind === "panel" &&
          (() => {
            const waits = waitingOn(threads).length;
            return (
              <IconButton
                label={waits ? `Plan, ${waits} need you` : "Plan"}
                aria-haspopup="dialog"
                aria-expanded={boardOpen}
                onClick={() => setBoardOpen(true)}
              >
                <ListTree />
                {waits > 0 && <StatusLed state="active" />}
              </IconButton>
            );
          })()}
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
      {boardOpen && phone && session?.kind === "panel" && (
        <Modal
          title="Plan"
          layout="sheet"
          className="side-sheet"
          close={() => setBoardOpen(false)}
        >
          <Board
            threads={threads}
            choose={(id) => {
              setBoardOpen(false);
              void choose(id);
            }}
            menu={menu}
            state={boardState}
            open={(target) => {
              setBoardOpen(false);
              openInspector(target);
            }}
          />
        </Modal>
      )}
    </>
  );
}
