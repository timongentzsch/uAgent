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

// The terminal's code (xterm.js) is fetched the first time it opens.
const terminalPanel = () => import("../features/terminal/terminal.tsx");
const presenterView = () => import("../features/files/presenter.tsx");
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

function App({ host, bindResult, reading }: HostAppProps) {
  const hosts = useHosts();
  const { command, manage } = useApi();
  const [page, setPage] = useState<Page>(keptPage.read);
  useEffect(() => reading(page === "chat"), [page, reading]);
  useEffect(() => keptPage.write(page), [page]);
  // The folder or workspace whose overview the folder page shows, and the
  // folder terminal the terminal page shows.
  const [place, setPlace] = useState("");
  const [shell, setShell] = useState<TerminalInfo | null>(null);
  // Which Library tab /memory or /skills asked for.
  const [libraryKind, setLibraryKind] = useState<LibraryKind | "">("");
  // Library and Scheduled sit above the conversation: back returns to it.
  useDismiss(page !== "chat", () => setPage("chat"));
  // What opens over the page: the conversations drawer, the app's dialogs,
  // the palette or shortcuts sheet, the image viewer, the inspector and the
  // /btw card.
  const [drawer, setDrawer] = useState(false);
  // The conversation's files: beside the chat, or a sheet on a small screen.
  const [filesOpen, setFilesOpen] = useState(false);
  // Where each conversation's Files pane is, kept while it is closed or
  // moves between the sheet and the docked pane (features/files/flow.ts).
  const [flows, setFlows] = useState<Record<string, Flow>>({});
  const setFlow = useCallback(
    (id: string, update: (flow: Flow) => Flow) =>
      setFlows((all) => ({ ...all, [id]: update(all[id] ?? START) })),
    [],
  );
  // A path cited in a conversation opens there, with a fresh history. The
  // open conversation and its folder, as of this render.
  const selectedRef = useRef("");
  const folderRef = useRef("");
  const openFile = useCallback(
    (target: FileTarget, id = selectedRef.current) => {
      setFlow(id, () => cite(target, folderRef.current));
      setFilesOpen(true);
    },
    [setFlow],
  );
  // The terminal under the chat.
  const [terminalOpen, setTerminalOpen] = useState(false);
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
  // The drawer belongs to the compact layout; a wider window drops it.
  useEffect(() => setDrawer(false), [compact]);
  useEdgeSwipe(compact, () => setDrawer(true));
  // The files sit beside the chat while its column has room, else in a sheet.
  const filesSheet = useNarrowColumn(shown) || compact;
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
  // A path asked for in another conversation (from the Library) opens there
  // once it is the one shown.
  const revealNext = useRef<FileTarget | null>(null);
  // biome-ignore lint/correctness/useExhaustiveDependencies: on each conversation chosen
  useEffect(() => {
    if (revealNext.current) openFile(revealNext.current, selected);
    revealNext.current = null;
  }, [selected]);
  // Every event that changes a session changes its catalogue entry too, so
  // the shell reads the entry and renders once per change; a snapshot's
  // metadata stands in only for a session the catalogue does not list yet.
  const listedSession = sessionById(catalogue.sessions, selected);
  const metadata = useSnapshots(snapshots, (all) =>
    listedSession ? undefined : all[selected]?.metadata,
  );
  const session = listedSession || metadata;
  selectedRef.current = selected;
  folderRef.current = session ? folderOf(session) : "";
  // The list leaves out empty conversations but the open one, also behind
  // the Library or a terminal. Which one is open matters only while it is
  // empty, so choosing among the others does not filter the list again.
  const keep = listedSession?.turns === 0 ? selected : "";
  const rows = useMemo(
    () => shownSessions(listed, keep, online),
    [listed, keep, online],
  );
  // Until the catalogue answers, the shell renders with placeholders in the
  // places data will fill; a conversation in the URL keeps its surfaces.
  const booting = authenticated === null;
  const opening = booting && !!selected;
  const draft = drafts[selected] || emptyDraft();
  // Uploads belong to their conversation: one in flight elsewhere never
  // holds this composer.
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
  const act: Act = useCallback(
    async <K extends CommandKind>(kind: K, fields?: CommandFields<K>) => {
      if (!online)
        throw new Error("Reconnect and refresh before sending commands.");
      if (fields?.request_id) correlate(fields.request_id);
      return command(kind, session, fields);
    },
    [online, session, correlate, command],
  );
  const choose = useCallback(
    async (id: string, owner = hosts.active) => {
      setNotice("");
      setPage("chat");
      const change = () => {
        // A conversation opens on its own host, which connects when needed;
        // `owner` names it while the list does not hold a new one yet.
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
  // A path in a place's Files pane, asked for from the Library: in the
  // place's latest conversation, or a new one where it has none.
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
  // The host context and the conversation menus must keep their functions;
  // they read the latest ones through a ref.
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

  // Forks any conversation from its menu, as /fork does the open one.
  const fork = useCallback(
    (item: Session) => latest.current.forkAndOpen(item).catch(report),
    [report],
  );
  // The open conversation's menu also holds the level its transcript shows.
  // Conversations and workspaces picked in the list, and what is done to
  // them all at once (features/sidebar, shared/selection.ts).
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
  // Alt+↑/↓: the conversation above or below in the list.
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
  // A folder's new terminal, shown at once.
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
  const toggleTerminal = useCallback(() => setTerminalOpen((on) => !on), []);
  // What the terminal or the files hand the conversation: added to its
  // draft, never sent. On a small screen they cover the chat, so they step
  // aside; the terminal's shells keep running and reattach when it opens.
  // A design's comments keep it open, for the next one.
  const addToChat = useCallback(
    (text: string, keep = false) => {
      updateDraft(selected, (current) => ({
        ...current,
        text: current.text ? `${current.text}\n${text}` : text,
      }));
      if (keep) return;
      if (compact) setTerminalOpen(false);
      if (filesSheet) setFilesOpen(false);
      focusComposer();
    },
    [updateDraft, selected, compact, filesSheet],
  );
  const toChat = page === "chat" && session && online ? addToChat : undefined;
  // Beside the chat, or in a sheet that closes itself.
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
  // Ends a folder's terminal; the one shown gives way to its folder.
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
                        swipeable={false}
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
                    // Every host's conversations: search spans the main.
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
// A presenter's window shows the deck's presenter view, not the app.
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
