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
} from "../shared/types.ts";
import { render, type ComponentChildren } from "preact";
import { useCallback, useEffect, useMemo, useState } from "preact/hooks";
import { emptyDraft, hasContent } from "../state/store.ts";
import { command } from "../state/api.ts";
import { ConversationContext, HostContext } from "../state/host-context.ts";
import { Modal, ErrorBoundary } from "../shared/ui.tsx";
import { ImageViewer, type ViewedImage } from "../shared/attachments.tsx";
import { ResizeHandle } from "../shared/side-pane.tsx";
import { useDismiss } from "../shared/dismiss.ts";
import Sidebar, { sessionOrder } from "../features/sidebar/sidebar.tsx";
import { ConversationMenu } from "./menus.tsx";
import { useShortcuts } from "../shared/shortcuts.ts";
import { nextIndex } from "../shared/listbox-nav.ts";
import { isRunning, sessionById, threadsOf } from "../state/attention.ts";
import { lockPageZoom, useCompact } from "../shared/layout.ts";
import { applyDevicePrefs } from "../shared/device-prefs.ts";
import { useLatest } from "../shared/use-latest.ts";
import { stored, tabStorage } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";

import { useHost } from "../state/use-host.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { DetailContext, detail as detailOf } from "../shared/verbosity.ts";
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
import type { FindRequest } from "../features/chat/find-bar.tsx";
import { useMessageActions } from "./use-message-actions.ts";
import Library from "../features/library/library.tsx";
import Scheduled from "../features/scheduled/scheduled.tsx";
import Pairing from "../features/pairing/pairing.tsx";
import Files from "../features/files/files.tsx";
import Spotlight, { ShortcutList } from "../features/search/spotlight.tsx";

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
const NO_LEVELS: string[] = [];
type Page = "chat" | "library" | "scheduled";
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
  // Which Library tab /memory or /skills asked for.
  const [libraryKind, setLibraryKind] = useState<"memory" | "skills">("memory");
  // Library and Scheduled sit above the conversation: back returns to it.
  useDismiss(page !== "chat", () => setPage("chat"));
  const compact = useCompact();
  // What opens over the page: the conversations drawer, the board sheet, the
  // app's dialogs, the palette or shortcuts sheet, the image viewer, the
  // inspector and the /btw card.
  const [drawer, setDrawer] = useState(false);
  // The conversation's files: beside the chat, or a sheet on a small screen.
  const [filesOpen, setFilesOpen] = useState(false);
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
    setVerbosity,
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
  // How much of the agent's work shows: one level for every conversation,
  // browser and terminal. Only rendering reads it, so a change restyles
  // every conversation where it stands.
  const detail = useMemo(
    () => detailOf(catalogue.verbosity),
    [catalogue.verbosity],
  );
  const levels = catalogue.verbosity?.order || NO_LEVELS;
  const changeDetail = useCallback(
    (level: string) => setVerbosity(level).catch(report),
    [setVerbosity, report],
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
  const { startConversation, newPlace, startIn, openPanel, projects } = places;
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
  // The other conversations of the open one's folder. A panel's chat
  // names who of them wrote what and shows each one's prompt.
  const threads = useMemo(
    () => threadsOf(catalogue.sessions, session?.cwd || ""),
    [catalogue.sessions, session?.cwd],
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
                level: detail.level,
                disabled: !online,
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
    const order = sessionOrder(listed, catalogue.spaces);
    if (!order.length) return;
    const at = order.findIndex((item) => item.id === selected);
    choose(order[nextIndex(at, key, order.length)].id);
  };
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
  };
  // Only for a paired device: the pairing screen has nothing to find.
  useShortcuts(authenticated ? shortcutActions : {});
  const navigate = useCallback((value: typeof page) => {
    setPage(value);
    setDrawer(false);
  }, []);
  const settings = useCallback(() => open({ type: "settings" }), [open]);
  const hostContext = useMemo(
    () => ({
      online,
      connection,
      report,
      store: snapshots,
      load,
      refresh,
      forget,
      setModal,
      showContext: (item?: Session) => latest.current.showContext(item),
    }),
    [online, connection, report, snapshots, load, refresh, forget],
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
      spaceMenu={places.spaceMenu}
      selected={page === "chat" ? selected : ""}
      unread={unread}
      choose={choose}
      menu={conversationMenu}
      settings={settings}
      create={newPlace}
      start={startIn}
      openPanel={openPanel}
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
                      threads={threads}
                      placeName={places.placeName}
                      browserAvailable={browserAvailable}
                      filesOpen={filesOpen}
                      setFilesOpen={setFilesOpen}
                      openDrawer={() => setDrawer(true)}
                      openPanel={openPanel}
                      open={open}
                      settings={settings}
                      menu={conversationMenu}
                      choose={choose}
                      openInspector={setInspector}
                    />
                    {filesOpen && compact && page === "chat" && session && (
                      <Modal
                        title="Files"
                        layout="sheet"
                        className="files-sheet"
                        close={() => setFilesOpen(false)}
                      >
                        <Files key={session.id} session={session} />
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
                      threads={session?.kind === "panel" ? threads : undefined}
                      opening={opening}
                      catalogue={catalogue}
                      outgoing={outgoing}
                      loadError={loadErrors[selected]}
                      menu={conversationMenu}
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
                          />
                        )
                      }
                      find={find}
                      closeFind={closeFind}
                    />
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
                size="narrow"
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
                    start={(cwd: string, panel?: boolean) =>
                      void startConversation(cwd, panel).catch(report)
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
