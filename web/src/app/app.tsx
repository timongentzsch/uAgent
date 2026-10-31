import type {
  JSONValue,
  Draft,
  Session,
  Space,
  Act,
  CommandKind,
  CommandFields,
  AppModal,
  InspectorTarget,
  TerminalInfo,
} from "../shared/types.ts";
import { render, type ComponentChildren } from "preact";
import { useCallback, useEffect, useMemo, useState } from "preact/hooks";
import { emptyDraft, hasContent } from "../state/store.ts";
import { command, manage } from "../state/api.ts";
import { ConversationContext, HostContext } from "../state/host-context.ts";
import { Modal, ErrorBoundary, Deferred, Spinner } from "../shared/ui.tsx";
import { ImageViewer, type ViewedImage } from "../shared/attachments.tsx";
import { ResizeHandle } from "../shared/side-pane.tsx";
import { useDismiss } from "../shared/dismiss.ts";
import Sidebar, { sessionOrder } from "../features/sidebar/sidebar.tsx";
import { livePins } from "../features/sidebar/pins.tsx";
import { ConversationMenu } from "./menus.tsx";
import { useShortcuts } from "../shared/shortcuts.ts";
import { nextIndex } from "../shared/listbox-nav.ts";
import { isRunning, sessionById } from "../state/attention.ts";
import { lockPageZoom, useCompact } from "../shared/layout.ts";
import { applyDevicePrefs } from "../shared/device-prefs.ts";
import { useLatest } from "../shared/use-latest.ts";
import { stored, tabStorage } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";

import { useHost } from "../state/use-host.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { DetailContext } from "../features/chat/node.tsx";
import { detailLevels } from "../shared/tree.ts";
import { devicePrefs } from "../shared/device-prefs.ts";
import { useStored } from "../shared/storage.ts";
import ChatPage from "./chat-page.tsx";
import ConversationHeader from "./conversation-header.tsx";
import Modals, { PreferencesContext } from "./modals.tsx";
import Toasts from "./toasts.tsx";
import { useDraftUploads } from "./use-draft-uploads.ts";
import { useBoot } from "./use-boot.ts";
import { useSideConversations } from "./use-side-conversations.ts";
import { usePlaces } from "./use-places.tsx";
import { useCommands } from "./use-commands.ts";
import { folderOf } from "../shared/display.ts";
import { focusComposer } from "../shared/navigation.ts";
import type { FindRequest } from "../features/chat/find-bar.tsx";
import { useMessageActions } from "./use-message-actions.ts";
import Library from "../features/library/library.tsx";
import Scheduled from "../features/scheduled/scheduled.tsx";
import FolderOverview from "../features/overview/overview.tsx";
import Pairing from "../features/pairing/pairing.tsx";
import Files from "../features/files/files.tsx";
import Spotlight, { ShortcutList } from "../features/search/spotlight.tsx";

// The terminal's code (xterm.js) is fetched the first time it opens.
const terminalPanel = () => import("../features/terminal/terminal.tsx");
const folderTerminal = () =>
  import("../features/terminal/terminal.tsx").then((module) => ({
    default: module.FolderTerminal,
  }));

// Own scroll restoration from the first paint. history restoration only
// covers the document; the early opt-out in index.html (<head>, before any
// module) keeps the browser from restoring the inner transcript scroller
// after our mount pin (the mobile reload jump). Repeated here as a
// backstop for mounts that precede it. The transcript hook owns all
// scrolling from here on.
if (typeof history !== "undefined") history.scrollRestoration = "manual";

// The shell: navigation, header, toasts and dialogs. It never reads a whole
// snapshot, only what it selects from one (the session's metadata, whether
// its agent browses, whether any turn waits), so a streamed frame re-renders
// the conversation (<ChatPage>), not the shell and its sidebar.
const NO_SPACES: Space[] = [];
export type Page = "chat" | "library" | "scheduled" | "folder" | "terminal";
// The page this tab showed, so a reload comes back to it.
const keptPage = stored<Page>(
  storageKeys.page,
  "chat",
  (value) =>
    value === "library" || value === "scheduled" ? (value as Page) : "chat",
  { where: tabStorage },
);

function App() {
  const [page, setPage] = useState<Page>(keptPage.read);
  useEffect(() => keptPage.write(page), [page]);
  // The folder or workspace whose overview the folder page shows, and the
  // folder terminal the terminal page shows.
  const [place, setPlace] = useState("");
  const [shell, setShell] = useState<TerminalInfo | null>(null);
  // Which Library tab /memory or /skills asked for.
  const [libraryKind, setLibraryKind] = useState<"memory" | "skills">("memory");
  // Library and Scheduled sit above the conversation: back returns to it.
  useDismiss(page !== "chat", () => setPage("chat"));
  const compact = useCompact();
  // What opens over the page: the conversations drawer, the app's dialogs,
  // the palette or shortcuts sheet, the image viewer, the inspector and the
  // /btw card.
  const [drawer, setDrawer] = useState(false);
  // The conversation's files: beside the chat, or a sheet on a small screen.
  const [filesOpen, setFilesOpen] = useState(false);
  // The terminal under the chat, and a folder asked to open one in.
  const [terminalOpen, setTerminalOpen] = useState(false);
  const [terminalRequest, setTerminalRequest] = useState<{
    cwd: string;
    at: number;
  } | null>(null);
  const [modal, setModal] = useState<AppModal | null>(null);
  // Search (Mod+K) or the shortcuts sheet (?).
  const [overlay, setOverlay] = useState<"search" | "shortcuts" | null>(null);
  // The open conversation's find bar: its words, and the row to start at
  // when search opened it there; `at` makes a repeated request a new one.
  const [find, setFind] = useState<FindRequest | null>(null);
  const closeFind = useCallback(() => setFind(null), []);
  // The one image viewer every tile opens.
  const [viewed, setViewed] = useState<ViewedImage | null>(null);
  const [inspector, setInspector] = useState<InspectorTarget | null>(null);
  // The drawer belongs to the compact layout; a wider window drops it.
  useEffect(() => setDrawer(false), [compact]);
  const open = useCallback((value: AppModal) => {
    setDrawer(false);
    setModal(value);
  }, []);
  const openSearch = useCallback(() => {
    setDrawer(false);
    setOverlay("search");
  }, []);
  const [notice, setNotice] = useState("");
  // The banner shows one line: a new notice replaces an older error.
  useEffect(() => {
    if (notice) setError("");
  }, [notice]);
  const onResult = useCallback((value: JSONValue, inspect: boolean) => {
    if (inspect) setModal({ type: "raw", value });
    else setNotice(typeof value === "string" ? value : JSON.stringify(value));
  }, []);
  const host = useHost(onResult, page === "chat");
  const {
    managementVersion,
    authenticated,
    online,
    connection,
    loadErrors,
    catalogue,
    listed,
    attention,
    upsertSession,
    snapshots,
    selected,
    setSelected,
    drafts,
    setDrafts,
    error,
    setError,
    unread,
    outgoing,
    setOutgoing,
    patchOutgoing,
    following,
    setFollowing,
    notifications,
    load,
    forget,
    refresh,
    report,
    updateView,
    correlate,
    reset,
  } = host;
  const { side, closeSide, ask, clearSides } = useSideConversations({
    selected,
    load,
    refresh,
    report,
  });
  const [busy, setBusy] = useState(false);
  const [notificationMode, setNotificationMode] = useState(false);
  // How much of the agent's work shows on this device, for every
  // conversation: its own level once chosen, else the host's. Choosing one
  // forgets the rows opened or closed here, so each starts at the new depth.
  const [chosen, setChosen] = useStored(devicePrefs.detail);
  const detail = chosen || catalogue.detail || "default";
  const levels = detailLevels as readonly string[];
  const changeDetail = useCallback(
    (level: string) => {
      devicePrefs.toggles.write({});
      setChosen(level);
    },
    [setChosen],
  );
  // Every event that changes a session changes its catalogue entry too, so
  // the shell reads the entry and renders once per change; a snapshot's
  // metadata stands in only for a session the catalogue does not list yet.
  const listedSession = sessionById(catalogue.sessions, selected);
  const metadata = useSnapshots(snapshots, (all) =>
    listedSession ? undefined : all[selected]?.metadata,
  );
  const session = listedSession || metadata;
  // Until the catalogue answers, the shell renders with placeholders in the
  // places data will fill; a conversation in the URL keeps its surfaces.
  const booting = authenticated === null;
  const opening = booting && !!selected;
  const draft = drafts[selected] || emptyDraft();
  // Uploads belong to their conversation: one in flight elsewhere never
  // holds this composer.
  const uploading = draft.files.some((file) => file.pending);
  const running = !!session && isRunning(session, online);
  const { updateDraft, attachAnnotated, upload } = useDraftUploads({
    session,
    online,
    selected,
    draft,
    uploading,
    setDrafts,
    report,
  });
  const setDraft = useCallback(
    (value: Draft, id = selected) => updateDraft(id, () => value),
    [updateDraft, selected],
  );
  const act: Act = useCallback(
    async <K extends CommandKind>(kind: K, fields?: CommandFields<K>) => {
      if (!online)
        throw new Error("Reconnect and refresh before sending commands.");
      if (fields?.request_id) correlate(fields.request_id);
      return command(kind, session, fields);
    },
    [online, session, correlate],
  );
  const choose = useCallback(
    async (id: string) => {
      setNotice("");
      setPage("chat");
      setSelected(id);
      setDrawer(false);
    },
    [setSelected],
  );
  const { browserAvailable, install, setInstall, update, attach } = useBoot({
    upload,
    page,
    choose,
    report,
    authenticated,
    online,
  });
  // The update banner holds its reload while a turn waits on a decision.
  const waiting = useSnapshots(
    snapshots,
    (all) => !!update && Object.values(all).some((item) => item.pending),
  );
  // Why an update must wait: unsent work (an upload is one) or a decision.
  const updateBlocked = Object.values(drafts).some(hasContent)
    ? "Send or copy unsent drafts first."
    : waiting
      ? "Wait for the running turn to finish."
      : "";
  const places = usePlaces({
    catalogue,
    upsertSession,
    choose,
    load,
    report,
    open,
    modal,
    setModal,
    setBusy,
  });
  const { startConversation, newPlace, startIn, startScratch, projects } =
    places;
  const { submit, deliver, forkAndOpen, showContext, runCommand } = useCommands(
    {
      host,
      session,
      running,
      busy,
      setBusy,
      draft,
      uploading,
      setDraft,
      updateDraft,
      act,
      choose,
      startConversation,
      setModal,
      open,
      setPage,
      setLibraryKind,
      compact,
      setDrawer,
      search: openSearch,
      setNotice,
      levels,
      changeDetail,
      askSide: ask,
    },
  );
  const chatActions = useMessageActions({
    online,
    selected,
    act,
    report,
    forkAndOpen,
    outgoing,
    session,
    deliver,
    busy,
    setOutgoing,
    patchOutgoing,
    updateDraft,
    snapshots,
    setInspector,
    setModal,
    choose,
  });
  // The host context and the conversation menus must keep their functions;
  // they read the latest ones through a ref.
  const latest = useLatest({ showContext, forkAndOpen });
  async function logout() {
    try {
      const registration = await navigator.serviceWorker?.getRegistration();
      await (await registration?.pushManager?.getSubscription())?.unsubscribe();
      await command("logout");
      clearSides();
      reset();
      setModal(null);
    } catch (failure) {
      report(failure);
    }
  }

  // Forks any conversation from its menu, as /fork does the open one.
  const fork = useCallback(
    (item: Session) => latest.current.forkAndOpen(item).catch(report),
    [report],
  );
  // The open conversation's menu also holds the level its transcript shows.
  const conversationMenu = useCallback(
    (item: Session, withDetail = false, extra?: ComponentChildren) => (
      <ConversationMenu
        item={item}
        fork={fork}
        find={
          withDetail
            ? () => setFind({ session: item.id, words: "", at: Date.now() })
            : undefined
        }
        notify={setNotice}
        open={open}
        detail={
          withDetail
            ? {
                levels,
                level: detail,
                change: changeDetail,
              }
            : undefined
        }
      >
        {extra}
      </ConversationMenu>
    ),
    [online, fork, load, report, open, detail, levels, changeDetail],
  );
  // Alt+↑/↓: the conversation above or below in the list.
  const step = (key: "ArrowUp" | "ArrowDown") => {
    const order = sessionOrder(listed, catalogue.spaces, online, pins);
    if (!order.length) return;
    const at = order.findIndex((item) => item.id === selected);
    choose(order[nextIndex(at, key, order.length)].id);
  };
  const terminalAvailable = !!catalogue.terminals;
  const showTerminal = useCallback((terminal: TerminalInfo) => {
    setShell(terminal);
    setPage("terminal");
    setDrawer(false);
  }, []);
  // A folder's new terminal, shown at once.
  const newTerminal = useCallback(
    (folder: string) =>
      void manage("terminal", { action: "open", cwd: folder, folder }).then(
        ({ id }) => id && showTerminal({ id, cwd: folder, folder }),
        report,
      ),
    [showTerminal, report],
  );
  const toggleTerminal = useCallback(() => setTerminalOpen((on) => !on), []);
  // A tab in a folder of the conversation's; on a phone the files sheet
  // makes way for it.
  const openTerminal = useCallback(
    (cwd: string) => {
      if (compact) setFilesOpen(false);
      setTerminalRequest({ cwd, at: Date.now() });
      setTerminalOpen(true);
    },
    [compact],
  );
  const filesTerminal =
    terminalAvailable && session
      ? (path: string) =>
          openTerminal(
            path ? `${folderOf(session)}/${path}` : folderOf(session),
          )
      : undefined;
  // What the terminal hands the conversation: added to its draft, never sent.
  // On a small screen the terminal covers the chat, so it steps aside; its
  // shells keep running and reattach when it opens again.
  const addToChat = useCallback(
    (text: string) => {
      updateDraft(selected, (current) => ({
        ...current,
        text: current.text ? `${current.text}\n${text}` : text,
      }));
      if (compact) setTerminalOpen(false);
      focusComposer();
    },
    [updateDraft, selected, compact],
  );
  const terminal = (
    <Deferred
      load={terminalPanel}
      fallback={<Spinner label="Opening the terminal…" />}
      cwd={session ? folderOf(session) : ""}
      request={terminalRequest}
      addToChat={page === "chat" && session && online ? addToChat : undefined}
      close={() => setTerminalOpen(false)}
      report={report}
    />
  );
  const shortcutActions = {
    // ⌘K again closes it, as Spotlight's own key does.
    search: () =>
      setOverlay((current) => (current === "search" ? null : "search")),
    // Only over an open conversation; elsewhere the browser's own find.
    find:
      page === "chat" && session
        ? () => setFind({ session: session.id, words: "", at: Date.now() })
        : undefined,
    shortcuts: () => setOverlay("shortcuts"),
    previous: () => step("ArrowUp"),
    next: () => step("ArrowDown"),
    terminal: terminalAvailable ? toggleTerminal : undefined,
  };
  // Only for a paired device: the pairing screen has nothing to find.
  useShortcuts(authenticated ? shortcutActions : {});
  const navigate = useCallback((value: typeof page) => {
    setPage(value);
    setDrawer(false);
  }, []);
  const overview = useCallback((cwd: string) => {
    setPlace(cwd);
    setPage("folder");
    setDrawer(false);
  }, []);
  const settings = useCallback(() => open({ type: "settings" }), [open]);
  const pins = useMemo(
    () => livePins(catalogue.pins, listed, catalogue.spaces),
    [catalogue.pins, listed, catalogue.spaces],
  );
  const hostContext = useMemo(
    () => ({
      online,
      connection,
      report,
      pins,
      store: snapshots,
      load,
      refresh,
      forget,
      setModal,
      showContext: (item?: Session) => latest.current.showContext(item),
    }),
    [online, connection, report, pins, snapshots, load, refresh, forget],
  );
  const conversation = useMemo(
    () => ({
      draft,
      setDraft,
      submit,
      act,
      upload: attach,
      uploading,
      busy,
      following,
      setFollowing,
    }),
    [
      draft,
      setDraft,
      submit,
      act,
      attach,
      uploading,
      busy,
      following,
      setFollowing,
    ],
  );
  const sidebar = (
    <Sidebar
      loading={booting}
      page={page}
      navigate={navigate}
      scheduledUnread={
        !!catalogue.scheduled?.runs?.some((run) => unread.has(run.session_id))
      }
      sessions={listed}
      needsYou={attention}
      spaces={catalogue.spaces || NO_SPACES}
      pins={pins}
      spaceMenu={places.spaceMenu}
      selected={page === "chat" ? selected : ""}
      unread={unread}
      choose={choose}
      menu={conversationMenu}
      settings={settings}
      create={newPlace}
      start={startIn}
      startScratch={startScratch}
      overview={overview}
      shownPlace={page === "folder" ? place : ""}
      terminals={catalogue.terminals}
      showTerminal={showTerminal}
      newTerminal={terminalAvailable ? newTerminal : undefined}
      shownTerminal={page === "terminal" ? shell?.id : ""}
      search={openSearch}
    />
  );

  return (
    <HostContext.Provider value={hostContext}>
      <ConversationContext.Provider value={conversation}>
        <DetailContext.Provider value={detail}>
          <ImageViewer.Provider value={setViewed}>
            <Toasts
              error={error}
              notice={notice}
              dismiss={() => {
                setError("");
                setNotice("");
              }}
              update={update}
              updateBlocked={updateBlocked}
            />
            {authenticated === false ? (
              <Pairing paired={refresh} report={report} />
            ) : (
              <>
                {/* First stop for a keyboard: past the sessions list. The hash
                routes sessions, so the link moves focus itself. */}
                <a
                  class="skip-link"
                  href="#conversation"
                  onClick={(event) => {
                    event.preventDefault();
                    document
                      .getElementById("conversation")
                      ?.focus({ preventScroll: true });
                  }}
                >
                  Skip to conversation
                </a>
                <div class="shell">
                  {!compact ? (
                    <aside
                      class="sidebar"
                      aria-label="Folders and conversations"
                    >
                      {sidebar}
                      <ResizeHandle
                        pane="sidebar"
                        label="Resize the sidebar"
                        edge="end"
                      />
                    </aside>
                  ) : (
                    drawer && (
                      <Modal
                        title="Conversations"
                        className="sidebar drawer"
                        close={() => setDrawer(false)}
                      >
                        {sidebar}
                      </Modal>
                    )
                  )}
                  <main class="conversation" id="conversation" tabIndex={-1}>
                    <ConversationHeader
                      page={page}
                      selected={selected}
                      session={session}
                      opening={opening}
                      booting={booting}
                      browserAvailable={browserAvailable}
                      terminal={
                        terminalAvailable
                          ? { open: terminalOpen, toggle: toggleTerminal }
                          : undefined
                      }
                      filesOpen={filesOpen}
                      setFilesOpen={setFilesOpen}
                      parent={sessionById(catalogue.sessions, session?.parent)}
                      openDrawer={() => setDrawer(true)}
                      choose={choose}
                      settings={settings}
                      menu={conversationMenu}
                    />
                    {filesOpen && compact && page === "chat" && session && (
                      <Modal
                        title="Files"
                        layout="sheet"
                        className="files-sheet"
                        close={() => setFilesOpen(false)}
                      >
                        <Files
                          key={session.id}
                          session={session}
                          openTerminal={filesTerminal}
                        />
                      </Modal>
                    )}
                    {page === "library" && (
                      <Library
                        key={libraryKind}
                        initialKind={libraryKind}
                        projects={projects}
                        cwd={session?.cwd || projects[0] || ""}
                        online={online}
                        version={managementVersion}
                      />
                    )}
                    {page === "folder" && (
                      <FolderOverview
                        key={place}
                        folder={place}
                        space={catalogue.spaces?.find(
                          (item) => item.path === place,
                        )}
                        sessions={catalogue.sessions}
                        choose={choose}
                        start={(cwd: string, text: string) =>
                          startConversation(cwd, "", text)
                        }
                        terminals={catalogue.terminals}
                        showTerminal={showTerminal}
                        newTerminal={
                          terminalAvailable ? newTerminal : undefined
                        }
                      />
                    )}
                    {page === "terminal" && shell && (
                      <Deferred
                        key={shell.id}
                        load={folderTerminal}
                        fallback={<Spinner label="Opening the terminal…" />}
                        terminal={shell}
                        closed={() => overview(shell.folder)}
                        report={report}
                      />
                    )}
                    {page === "scheduled" && (
                      <Scheduled
                        projects={projects}
                        cwd={session?.cwd || projects[0] || ""}
                        online={online}
                        scheduled={catalogue.scheduled}
                        unread={unread}
                        choose={choose}
                        refresh={refresh}
                      />
                    )}
                    <ChatPage
                      active={page === "chat"}
                      historyKey={
                        page === "chat" ? `${page}:${selected}` : `page:${page}`
                      }
                      selected={selected}
                      session={session}
                      opening={opening}
                      catalogue={catalogue}
                      outgoing={outgoing}
                      loadError={loadErrors[selected]}
                      newPlace={newPlace}
                      updateView={updateView}
                      choose={choose}
                      side={side}
                      closeSide={closeSide}
                      actions={chatActions}
                      setInspector={setInspector}
                      files={
                        filesOpen &&
                        !compact &&
                        session && (
                          <Files
                            key={session.id}
                            session={session}
                            close={() => setFilesOpen(false)}
                            openTerminal={filesTerminal}
                          />
                        )
                      }
                      find={find}
                      closeFind={closeFind}
                    />
                    {terminalOpen && terminalAvailable && !compact && (
                      <section class="terminal-dock" aria-label="Terminal">
                        <ResizeHandle
                          pane="terminal"
                          label="Resize the terminal"
                          edge="top"
                        />
                        {terminal}
                      </section>
                    )}
                    {terminalOpen && terminalAvailable && compact && (
                      // A docked strip under the chat is too small to type in
                      // once the keyboard is up: on a small screen the
                      // terminal takes the whole display, as the browser does.
                      <Modal
                        title="Terminal"
                        layout="sheet"
                        className="terminal-sheet"
                        header={false}
                        close={() => setTerminalOpen(false)}
                      >
                        {terminal}
                      </Modal>
                    )}
                  </main>
                </div>
              </>
            )}
            {overlay && (
              // Keyed: an action that opens the other one replaces this dialog
              // instead of closing it under the new one.
              <Modal
                key={overlay}
                title={overlay === "search" ? "Search" : "Keyboard shortcuts"}
                layout={overlay === "search" ? "spotlight" : "sheet"}
                size="s"
                header={overlay !== "search"}
                close={() => setOverlay(null)}
              >
                {overlay === "search" ? (
                  <Spotlight
                    sessions={listed}
                    commands={catalogue.commands || []}
                    place={session && folderOf(session)}
                    choose={choose}
                    find={(id: string, words: string, sequence: number) => {
                      void choose(id);
                      setFind({ session: id, words, sequence, at: Date.now() });
                    }}
                    start={(cwd: string) =>
                      void startConversation(cwd).catch(report)
                    }
                    run={runCommand}
                    settings={(section: string) =>
                      open({ type: "settings", section })
                    }
                    actions={{ ...shortcutActions, search: undefined }}
                  />
                ) : (
                  <ShortcutList />
                )}
              </Modal>
            )}
            <PreferencesContext.Provider
              value={{
                install,
                setInstall,
                update,
                updateBlocked: !!updateBlocked,
                notificationMode,
                setNotificationMode,
                notifications,
                logout,
              }}
            >
              <Modals
                modal={modal}
                inspector={inspector}
                setInspector={setInspector}
                viewed={viewed}
                setViewed={setViewed}
                annotate={session && online ? attachAnnotated : undefined}
                selected={selected}
                session={session}
                catalogue={catalogue}
                compact={compact}
                managementVersion={managementVersion}
                places={places}
              />
            </PreferencesContext.Provider>
          </ImageViewer.Provider>
        </DetailContext.Provider>
      </ConversationContext.Provider>
    </HostContext.Provider>
  );
}
lockPageZoom();
applyDevicePrefs();
render(
  <ErrorBoundary>
    <App />
  </ErrorBoundary>,
  document.getElementById("app")!,
);
