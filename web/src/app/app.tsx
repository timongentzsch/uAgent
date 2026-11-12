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
  FileTarget,
  LibraryKind,
  LibraryPlace,
} from "../shared/types.ts";
import { render, type ComponentChildren } from "preact";
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import { emptyDraft, hasContent } from "../state/store.ts";
import { useApi, localApi } from "../state/api.ts";
import { ConversationContext, HostContext } from "../state/host-context.ts";
import { Modal, ErrorBoundary, Deferred, Spinner } from "../shared/ui.tsx";
import { ImageViewer, type ViewedImage } from "../shared/attachments.tsx";
import { ResizeHandle } from "../shared/side-pane.tsx";
import { settled, useDismiss } from "../shared/dismiss.ts";
import { slide } from "../shared/motion.ts";
import { useEdgeSwipe } from "../shared/swipe.ts";
import Sidebar, { sessionOrder } from "../features/sidebar/sidebar.tsx";
import { livePins } from "../features/sidebar/pins.tsx";
import { ConversationMenu, TerminalMenu } from "./menus.tsx";
import { useShortcuts } from "../shared/shortcuts.ts";
import { nextIndex } from "../shared/listbox-nav.ts";
import { isRunning, sessionById, shownSessions } from "../state/attention.ts";
import { lockPageZoom, useCompact, useNarrowColumn } from "../shared/layout.ts";
import { applyDevicePrefs } from "../shared/device-prefs.ts";
import { useWakeLock } from "../shared/wake-lock.ts";
import { useLatest } from "../shared/use-latest.ts";
import { stored, tabStorage } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";

import HostsShell, { type HostAppProps } from "./hosts.tsx";
import { useHosts } from "../state/sources.ts";
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
import { START, cite, type Flow } from "../features/files/flow.ts";
import { useSelection } from "../shared/selection.ts";
import { presenterDeck } from "../features/files/present.ts";
import Spotlight, { ShortcutList } from "../features/search/spotlight.tsx";

const terminalPanel = () => import("../features/terminal/terminal.tsx");
const presenterView = () => import("../features/files/presenter.tsx");
const folderTerminal = () =>
  import("../features/terminal/terminal.tsx").then((module) => ({
    default: module.FolderTerminal,
  }));

// The browser must not restore the inner transcript scroller after the mount
// pin. index.html opts out first; this is the backstop.
if (typeof history !== "undefined") history.scrollRestoration = "manual";

const NO_SPACES: Space[] = [];
export type Page = "chat" | "library" | "scheduled" | "folder" | "terminal";
const keptPage = stored<Page>(
  storageKeys.page,
  "chat",
  (value) =>
    value === "library" || value === "scheduled" ? (value as Page) : "chat",
  { where: tabStorage },
);

// The shell never reads a whole snapshot, only what it selects from one, so
// a streamed frame re-renders <ChatPage>, not the shell and its sidebar.
function App({ host, bindResult, reading }: HostAppProps) {
  const hosts = useHosts();
  const { command, manage } = useApi();
  const [page, setPage] = useState<Page>(keptPage.read);
  useEffect(() => reading(page === "chat"), [page, reading]);
  useEffect(() => keptPage.write(page), [page]);
  const [place, setPlace] = useState("");
  const [shell, setShell] = useState<TerminalInfo | null>(null);
  const [libraryKind, setLibraryKind] = useState<LibraryKind | "">("");
  useDismiss(page !== "chat", () => setPage("chat"));
  const [drawer, setDrawer] = useState(false);
  const [filesOpen, setFilesOpen] = useState(false);
  const [flows, setFlows] = useState<Record<string, Flow>>({});
  const setFlow = useCallback(
    (id: string, update: (flow: Flow) => Flow) =>
      setFlows((all) => ({ ...all, [id]: update(all[id] ?? START) })),
    [],
  );
  const selectedRef = useRef("");
  const folderRef = useRef("");
  const openFile = useCallback(
    (target: FileTarget, id = selectedRef.current) => {
      setFlow(id, () => cite(target, folderRef.current));
      setFilesOpen(true);
    },
    [setFlow],
  );
  // Hidden (false), its panel stays and its shells run on; none started
  // without a value.
  const [terminalShown, setTerminalShown] = useState<boolean>();
  const [modal, setModal] = useState<AppModal | null>(null);
  const [overlay, setOverlay] = useState<"search" | "shortcuts" | null>(null);
  const [find, setFind] = useState<FindRequest | null>(null);
  const closeFind = useCallback(() => setFind(null), []);
  const [viewed, setViewed] = useState<ViewedImage | null>(null);
  const [inspector, setInspector] = useState<InspectorTarget | null>(null);
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
    if (notice) host.setError("");
  }, [notice, host.setError]);
  const onResult = useCallback((value: JSONValue, inspect: boolean) => {
    if (inspect) setModal({ type: "raw", value });
    else setNotice(typeof value === "string" ? value : JSON.stringify(value));
  }, []);
  useEffect(() => bindResult(onResult), [bindResult, onResult]);
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
  // The shell is absent while the device pairs; its layout is read again
  // when it appears.
  const shown = authenticated !== false;
  const compact = useCompact(shown);
  useEffect(() => setDrawer(false), [compact]);
  useEdgeSwipe(compact, () => setDrawer(true));
  const filesSheet = useNarrowColumn(shown) || compact;
  const { side, closeSide, ask, clearSides } = useSideConversations({
    selected,
    load,
    refresh,
    report,
  });
  const [busy, setBusy] = useState(false);
  const [notificationMode, setNotificationMode] = useState(false);
  // Choosing a level forgets the rows opened or closed on this device.
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
  // A path asked for in another conversation opens once that one is shown.
  const revealNext = useRef<FileTarget | null>(null);
  // biome-ignore lint/correctness/useExhaustiveDependencies: on each conversation chosen
  useEffect(() => {
    if (revealNext.current) openFile(revealNext.current, selected);
    revealNext.current = null;
  }, [selected]);
  // The catalogue entry changes with every session event; a snapshot's
  // metadata stands in only for a session not listed yet.
  const listedSession = sessionById(catalogue.sessions, selected);
  const metadata = useSnapshots(snapshots, (all) =>
    listedSession ? undefined : all[selected]?.metadata,
  );
  const session = listedSession || metadata;
  selectedRef.current = selected;
  folderRef.current = session ? folderOf(session) : "";
  // Only an empty open conversation is passed on, so choosing among the
  // others does not filter the list again.
  const keep = listedSession?.turns === 0 ? selected : "";
  const rows = useMemo(
    () => shownSessions(listed, keep, online),
    [listed, keep, online],
  );
  // Until the catalogue answers, placeholders fill the shell.
  const booting = authenticated === null;
  const opening = booting && !!selected;
  const draft = drafts[selected] || emptyDraft();
  const uploading = draft.files.some((file) => file.pending);
  const running = !!session && isRunning(session, online);
  useWakeLock(running);
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
  // Commands on their way, by request id. One that acts on another (a
  // queued message taken back or inserted) waits for that one's answer: the
  // host may not know it yet, and would say it was already delivered.
  const going = useRef(new Map<string, Promise<unknown>>());
  const act: Act = useCallback(
    async <K extends CommandKind>(kind: K, fields?: CommandFields<K>) => {
      if (!online)
        throw new Error("Reconnect and refresh before sending commands.");
      const { request_id, target_id } = (fields ?? {}) as {
        request_id?: string;
        target_id?: string;
      };
      if (request_id) correlate(request_id);
      await going.current.get(target_id ?? "");
      const sent = command(kind, session, fields);
      if (request_id)
        going.current.set(
          request_id,
          sent.catch(() => {}).finally(() => going.current.delete(request_id)),
        );
      return sent;
    },
    [online, session, correlate, command],
  );
  const choose = useCallback(
    async (id: string, owner = hosts.active) => {
      setNotice("");
      setPage("chat");
      const change = () => {
        // `owner` names the host of a conversation the list does not hold yet.
        if (id) hosts.open(owner, id);
        else setSelected("", owner);
        setDrawer(false);
      };
      if (id === selected) change();
      else slide(false, id, change);
    },
    [selected, setSelected, hosts.open, hosts.active],
  );
  const { browserAvailable, install, setInstall, update, attach } = useBoot({
    upload,
    page,
    choose,
    report,
    authenticated,
    online,
  });
  const waiting = useSnapshots(
    snapshots,
    (all) => !!update && Object.values(all).some((item) => item.pending),
  );
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
  const showInPlace = useCallback(
    (place: LibraryPlace, target: FileTarget) => {
      setFilesOpen(true);
      if (place.session === selected) {
        setPage("chat");
        openFile(target);
        return;
      }
      revealNext.current = target;
      if (place.session) void choose(place.session);
      else void startConversation(place.path).catch(report);
    },
    [selected, openFile, choose, startConversation, report],
  );
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
    openFile,
  });
  // Read through a ref, so the host context and the menus keep their functions.
  const latest = useLatest({ showContext, forkAndOpen });
  async function logout() {
    try {
      const registration = await navigator.serviceWorker?.getRegistration();
      await (await registration?.pushManager?.getSubscription())?.unsubscribe();
      await localApi.command("logout");
      clearSides();
      reset();
      setModal(null);
    } catch (failure) {
      report(failure);
    }
  }

  const fork = useCallback(
    (item: Session) => latest.current.forkAndOpen(item).catch(report),
    [report],
  );
  const selection = useSelection();
  const latestSelection = useLatest(selection);
  const actOnSelection = useCallback(
    (
      action: "move" | "close" | "delete",
      sessions: Session[],
      spaces: Space[],
    ) => {
      if (action === "move")
        open({ type: "move", session: sessions[0], sessions });
      else if (action === "delete")
        open({ type: "delete-many", sessions, spaces });
      else
        void (async () => {
          for (const item of sessions)
            if (item.generation) await command("close", item).catch(report);
          await refresh();
        })();
    },
    [open, report, refresh, command],
  );
  const conversationMenu = useCallback(
    (
      item: Session,
      withDetail = false,
      extra?: { top?: ComponentChildren; more?: ComponentChildren },
    ) => (
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
        more={extra?.more}
        select={
          withDetail ? undefined : () => latestSelection.current.start(item.id)
        }
      >
        {extra?.top}
      </ConversationMenu>
    ),
    [online, fork, load, report, open, detail, levels, changeDetail],
  );
  const step = (key: "ArrowUp" | "ArrowDown") => {
    const order = sessionOrder(
      rows,
      catalogue.spaces,
      online,
      pins,
      hosts.active,
    );
    if (!order.length) return;
    const at = order.findIndex((item) => item.id === selected);
    choose(order[nextIndex(at, key, order.length)].id);
  };
  const terminalAvailable = !!catalogue.terminals;
  const terminalStarted = terminalAvailable && terminalShown !== undefined;
  const showTerminal = useCallback((terminal: TerminalInfo) => {
    setShell(terminal);
    setPage("terminal");
    setDrawer(false);
  }, []);
  const { destination } = hosts;
  const followedDestination = useRef<typeof destination>(null);
  useEffect(() => {
    if (
      !destination ||
      destination.id !== hosts.active ||
      followedDestination.current === destination
    )
      return;
    const terminal = catalogue.terminals?.find(
      (item) => item.id === destination.terminal,
    );
    if (destination.terminal && !terminal) return;
    followedDestination.current = destination;
    // After a step back still under way (out of the drawer it was picked
    // in), so a page or dialog this opens gets an entry of its own.
    settled(() => {
      setDrawer(false);
      if (destination.create) newPlace(destination.create);
      else if (terminal) showTerminal(terminal);
      else if (destination.place) {
        setPlace(destination.place);
        setPage("folder");
      } else if (destination.session) setPage("chat");
    });
  }, [destination, hosts.active, showTerminal, catalogue.terminals, newPlace]);
  const newTerminal = useCallback(
    (folder: string) =>
      void manage("terminal", { action: "open", cwd: folder, folder }).then(
        ({ id, terminals }) => {
          const opened = terminals.find((item) => item.id === id);
          if (opened) showTerminal(opened);
        },
        report,
      ),
    [showTerminal, report, manage],
  );
  const toggleTerminal = useCallback(() => setTerminalShown((on) => !on), []);
  const hideTerminal = useCallback(
    () => setTerminalShown((on) => on && false),
    [],
  );
  // Added to the draft, never sent. What covers the chat on a small screen
  // steps aside, unless `keep`.
  const addToChat = useCallback(
    (text: string, keep = false) => {
      updateDraft(selected, (current) => ({
        ...current,
        text: current.text ? `${current.text}\n${text}` : text,
      }));
      if (keep) return;
      if (compact) hideTerminal();
      if (filesSheet) setFilesOpen(false);
      focusComposer();
    },
    [updateDraft, selected, compact, filesSheet, hideTerminal],
  );
  const toChat = page === "chat" && session && online ? addToChat : undefined;
  const files = session && (
    <Files
      key={session.id}
      session={session}
      close={filesSheet ? undefined : () => setFilesOpen(false)}
      flow={flows[session.id] ?? START}
      setFlow={(update) => setFlow(session.id, update)}
      addToChat={toChat}
      pictures={browserAvailable}
    />
  );
  const terminal = (
    <Deferred
      load={terminalPanel}
      fallback={<Spinner label="Opening the terminal…" />}
      cwd={session ? folderOf(session) : ""}
      addToChat={toChat}
      shown={!!terminalShown}
      hide={hideTerminal}
      close={() => setTerminalShown(undefined)}
      report={report}
    />
  );
  const shortcutActions = {
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
  const closeTerminal = useCallback(
    (terminal: TerminalInfo) =>
      void manage("terminal", { action: "close", id: terminal.id }).then(() => {
        if (page === "terminal" && shell?.id === terminal.id)
          overview(terminal.folder);
      }, report),
    [page, shell, overview, report, manage],
  );
  const terminalMenu = useCallback(
    (terminal: TerminalInfo) => (
      <TerminalMenu terminal={terminal} open={open} close={closeTerminal} />
    ),
    [open, closeTerminal],
  );
  const settings = useCallback(() => open({ type: "settings" }), [open]);
  const pins = useMemo(
    () =>
      livePins(
        catalogue.pins,
        listed,
        catalogue.spaces,
        catalogue.terminals || [],
      ),
    [catalogue.pins, listed, catalogue.spaces, catalogue.terminals],
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
      sessions={rows}
      needsYou={attention}
      spaces={catalogue.spaces || NO_SPACES}
      pins={pins}
      spaceMenu={places.spaceMenu}
      selection={selection}
      act={actOnSelection}
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
      terminalMenu={terminalMenu}
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
                {/* The hash routes sessions: the link moves focus itself. */}
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
                          ? { open: !!terminalShown, toggle: toggleTerminal }
                          : undefined
                      }
                      filesOpen={filesOpen}
                      setFilesOpen={setFilesOpen}
                      outgoing={outgoing}
                      parent={sessionById(catalogue.sessions, session?.parent)}
                      openDrawer={() => setDrawer(true)}
                      choose={choose}
                      settings={settings}
                      menu={conversationMenu}
                    />
                    {filesOpen && filesSheet && page === "chat" && session && (
                      <Modal
                        title="Files"
                        layout="sheet"
                        className="files-sheet"
                        header={false}
                        close={() => setFilesOpen(false)}
                      >
                        {files}
                      </Modal>
                    )}
                    {page === "library" && (
                      <Library
                        key={libraryKind}
                        initialKind={libraryKind}
                        online={online}
                        version={managementVersion}
                        openFiles={showInPlace}
                        choose={choose}
                      />
                    )}
                    {page === "folder" && (
                      <FolderOverview
                        key={`${hosts.active}:${place}`}
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
                        close={closeTerminal}
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
                      files={filesOpen && !filesSheet && files}
                      find={find}
                      closeFind={closeFind}
                    />
                    {terminalStarted && !compact && (
                      <section
                        class="terminal-dock"
                        aria-label="Terminal"
                        hidden={!terminalShown}
                      >
                        <ResizeHandle
                          pane="terminal"
                          label="Resize the terminal"
                          edge="top"
                        />
                        {terminal}
                      </section>
                    )}
                    {terminalStarted && compact && (
                      // A docked strip is too small to type in once the
                      // keyboard is up.
                      <Modal
                        title="Terminal"
                        layout="sheet"
                        className="terminal-sheet"
                        header={false}
                        swipeable={false}
                        hidden={!terminalShown}
                        close={hideTerminal}
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
                    sessions={hosts.sources.flatMap(
                      (source) => source.state.listed,
                    )}
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
const presenting = presenterDeck();
render(
  <ErrorBoundary>
    {presenting ? (
      <Deferred
        load={presenterView}
        fallback={<Spinner label="Opening the presenter view…" />}
        deck={presenting}
      />
    ) : (
      <HostsShell App={App} />
    )}
  </ErrorBoundary>,
  document.getElementById("app")!,
);
