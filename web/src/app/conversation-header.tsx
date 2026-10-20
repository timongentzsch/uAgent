import type { ComponentChildren } from "preact";
import { useState } from "preact/hooks";
import type { AppModal, Session } from "../shared/types.ts";
import { Button, IconButton, Modal } from "../shared/ui.tsx";
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
// and the conversation's menu.
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
  addMember,
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
  menu: (item: Session, withDetail?: boolean) => ComponentChildren;
  choose: (id: string) => Promise<void>;
  addMember: () => void;
}) {
  const { store, setModal } = useHostContext();
  const compact = useCompact();
  const phone = usePhone();
  // On a phone a panel's board slides in from the right on demand
  // instead of taking the top of its chat.
  const [boardOpen, setBoardOpen] = useState(false);
  // The browser icon shows when this conversation's agent is using it.
  const browsing = useSnapshots(store, (all) => isBrowsing(all[selected]));
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
              `Panel · ${placeName(session.cwd)}`
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
                ‹ Panel · {placeName(session.folder)}
              </Button>
            </p>
          )}
          {page === "chat" && session?.kind === "panel" && (
            <PanelHelp
              editInstructions={() => open({ type: "instructions" })}
            />
          )}
        </div>
        {browserAvailable && (
          <IconButton
            label={browsing ? "Open browser, agent working" : "Open browser"}
            class="browser-toggle"
            disabled={booting}
            onClick={() => setModal({ type: "browser" })}
          >
            <Globe2 />
            {browsing && <StatusLed state="running" />}
          </IconButton>
        )}
        {/* Held in place while the session loads, inert. */}
        {page === "chat" && (session || opening) && (
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
                label={waits ? `Board, ${waits} need you` : "Board"}
                aria-haspopup="dialog"
                aria-expanded={boardOpen}
                onClick={() => setBoardOpen(true)}
              >
                <ListTree />
                {waits > 0 && <StatusLed state="active" />}
              </IconButton>
            );
          })()}
        {compact && (
          <div>
            <IconButton label="Settings" onClick={settings}>
              <Settings />
            </IconButton>
          </div>
        )}
        {page === "chat" &&
          (session ? (
            menu(session, true)
          ) : opening ? (
            // Inert until the session is known.
            <IconButton label="Conversation menu" disabled>
              <Ellipsis />
            </IconButton>
          ) : null)}
      </header>
      {boardOpen && phone && session?.kind === "panel" && (
        <Modal
          title="Board"
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
            add={() => {
              setBoardOpen(false);
              addMember();
            }}
          />
        </Modal>
      )}
    </>
  );
}
