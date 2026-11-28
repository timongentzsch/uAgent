import { SETTINGS_AT } from "../shared/settings-topics.ts";
import { failure } from "../shared/types.ts";
import type { FileTarget } from "../shared/file-types.ts";
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
  FileOrigin,
  LibraryKind,
  LibraryPlace,
} from "../shared/types.ts";
import { render, type ComponentChildren } from "preact";
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import { hasContent } from "../state/store.ts";
import { useApi, localApi } from "../state/api.ts";
import { ConversationContext, HostContext } from "../state/host-context.ts";
import {
  Modal,
  ErrorBoundary,
  Deferred,
  Spinner,
  copyText,
} from "../shared/ui.tsx";
import { ImageViewer, type ViewedImage } from "../shared/attachments.tsx";
import { ResizeHandle } from "../shared/side-pane.tsx";
import {
  closeTopLayer,
  settled,
  underLayers,
  useDismiss,
} from "../shared/dismiss.ts";
import { slide } from "../shared/motion.ts";
import { useEdgeSwipe } from "../shared/swipe.ts";
import Sidebar, {
  ListActions,
  sessionOrder,
} from "../features/sidebar/sidebar.tsx";
import { livePins } from "../features/sidebar/pins.tsx";
import { ConversationMenu, TerminalMenu } from "./menus.tsx";
import { useShortcuts } from "../shared/shortcuts.ts";
import { nextIndex } from "../shared/listbox-nav.ts";
import {
  isRunning,
  isSideChat,
  sessionById,
  shownSessions,
} from "../state/attention.ts";
import {
  lockPageZoom,
  useCompact,
  useNarrowColumn,
  useRoomyColumn,
} from "../shared/layout.ts";
import { applyDevicePrefs } from "../shared/device-prefs.ts";
import { useWakeLock } from "../shared/wake-lock.ts";
import { useLatest } from "../shared/use-latest.ts";
import { stored, tabStorage } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";

import HostsShell, { type HostAppProps } from "./hosts.tsx";
import { useHosts } from "../state/sources.ts";
import { useStoreValue } from "../state/value-store.ts";
import { DetailContext } from "../features/chat/node.tsx";
import { detailLevels } from "../shared/tree.ts";
import { devicePrefs } from "../shared/device-prefs.ts";
import { useStored } from "../shared/storage.ts";
import ChatPage, { startFieldId } from "./chat-page.tsx";
import ConversationHeader from "./conversation-header.tsx";
import Modals, { PreferencesContext } from "./modals.tsx";
import Toasts from "./toasts.tsx";
import { useDraftUploads } from "./use-draft-uploads.ts";
import { useBoot } from "./use-boot.ts";
import { useSideConversations } from "./use-side-conversations.ts";
import { usePlaces } from "./use-places.tsx";
import { useCommands } from "./use-commands.ts";
import { folderOf } from "../shared/display.ts";
import {
  focusComposer,
  fileFromURL,
  settingsFromURL,
  artifactURL,
  withoutFileAddress,
} from "../shared/navigation.ts";
import { useLinkedFile } from "./use-linked-file.ts";
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
  const { api, routes, command, manage } = useApi();
  const [page, setPage] = useState<Page>(() =>
    fileFromURL() ? "chat" : keptPage.read(),
  );
  useEffect(() => reading(page === "chat"), [page, reading]);
  useEffect(() => keptPage.write(page), [page]);
  const [place, setPlace] = useState("");
  const [shell, setShell] = useState<TerminalInfo | null>(null);
  const [libraryKind, setLibraryKind] = useState<LibraryKind | "">("");
  useDismiss(page !== "chat", () => setPage("chat"));
  const [drawer, setDrawer] = useState(false);
  const [filesOpen, setFilesOpen] = useState<boolean | "expanded">(false);
  const [flows, setFlows] = useState<Record<string, Flow>>({});
  const setFlow = useCallback(
    (id: string, update: (flow: Flow) => Flow) =>
      setFlows((all) => ({ ...all, [id]: update(all[id] ?? START) })),
    [],
  );
  const selectedRef = useRef("");
  const folderRef = useRef("");
  const flowRef = useRef("");
  const openHere = useCallback(
    (target: FileTarget) => {
      setFlow(flowRef.current, () => cite(target, folderRef.current));
      setFilesOpen(true);
    },
    [setFlow],
  );
  // Hidden (false), its panel stays and its shells run on; none started
  // without a value.
  const [terminalExpanded, setTerminalExpanded] = useState(false);
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
    snapshots,
    selected,
    setSelected,
    drafts,
    error,
    errorCode,
    setError,
    unread,
    outgoing,
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
  const openDrawer = useCallback(() => setDrawer(true), []);
  useEdgeSwipe(compact, () => closeTopLayer() || openDrawer());
  const filesSheet = useNarrowColumn(shown) || compact;
  useDismiss(filesOpen === "expanded" && !filesSheet && page === "chat", () =>
    setFilesOpen(true),
  );
  const roomy = useRoomyColumn(shown);
  const side = useSideConversations(host);
  // A side chat is shown as one wherever it is opened from (search, Needs
  // you, a notification, an address): where the column fits, as its
  // conversation with its tab in front; the page is for layouts without.
  const opened = sessionById(catalogue.sessions, selected);
  const sideOf = opened && isSideChat(opened) ? opened.parent : undefined;
  // The side chats have a column where it fits beside the chat (and beside
  // Files, when that is open); elsewhere each is a page of its own.
  const sideColumn = !filesSheet && (!filesOpen || roomy);
  // biome-ignore lint/correctness/useExhaustiveDependencies: once per side chat opened
  useLayoutEffect(() => {
    if (!sideOf || !sideColumn) return;
    side.show(selected, sideOf);
    setSelected(sideOf, undefined, true);
  }, [sideOf, sideColumn, selected]);
  const [busy, setBusy] = useState(false);
  const [notificationMode, setNotificationMode] = useState(false);
  // Choosing a level forgets the rows opened or closed on this device.
  const [chosen, setChosen] = useStored(devicePrefs.detail);
  const detail = chosen || catalogue.detail || detailLevels[0];
  const levels = detailLevels as readonly string[];
  const changeDetail = useCallback(
    (level: string) => {
      devicePrefs.toggles.write({});
      setChosen(level);
    },
    [setChosen],
  );
  const [thinkingChosen, chooseThinking] = useStored(devicePrefs.thinking);
  const thinking = thinkingChosen
    ? thinkingChosen === "show"
    : !!catalogue.thinking;
  const display = useMemo(() => ({ detail, thinking }), [detail, thinking]);
  // What the header and the conversation menu change it with.
  const detailControl = useMemo(
    () => ({
      levels,
      level: detail,
      change: changeDetail,
      thinking,
      showThinking: (shown: boolean) => chooseThinking(shown ? "show" : "hide"),
    }),
    [levels, detail, changeDetail, thinking, chooseThinking],
  );
  // The catalogue entry changes with every session event; a snapshot's
  // metadata stands in only for a session not listed yet.
  const listedSession = sessionById(catalogue.sessions, selected);
  const metadata = useStoreValue(snapshots, (all) =>
    listedSession ? undefined : all[selected]?.metadata,
  );
  const session = listedSession || metadata;
  selectedRef.current = selected;
  folderRef.current = session ? folderOf(session) : "";
  // A move invalidates this workspace's file data and navigation history.
  const fileScope = JSON.stringify([
    selected,
    session?.file_origin || folderRef.current,
  ]);
  flowRef.current = fileScope;
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
  const running = !!session && isRunning(session, online);
  useWakeLock(running);
  const { updateDraft, attachAnnotated, upload } = useDraftUploads({ host });
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
      if (id === selectedRef.current) change();
      else slide(false, id, change);
    },
    [setSelected, hosts.open, hosts.active],
  );
  const { browserAvailable, install, setInstall, update, attach } = useBoot({
    host,
    upload,
    page,
    choose,
  });
  const waiting = useStoreValue(
    snapshots,
    (all) => !!update && Object.values(all).some((item) => item.pending),
    !!update,
  );
  const unsentDraft = useStoreValue(
    drafts,
    (all) => !!update && Object.values(all).some(hasContent),
    !!update,
  );
  const updateBlocked = unsentDraft
    ? "Send or copy unsent drafts first."
    : waiting
      ? "Wait for the running turn to finish."
      : "";
  const places = usePlaces({
    host,
    upload,
    choose,
    open,
    modal,
    setModal,
    setBusy,
  });
  const {
    startConversation,
    newPlace,
    startIn,
    startUnfiled,
    redirect,
    projects,
  } = places;
  // The home screen, ready to type on where the field takes the focus on
  // its own (StartField): not with the list in a drawer, a phone's layout,
  // where a keyboard would cover the lists.
  const home = useCallback(() => {
    void choose("").then(() => {
      if (!compact)
        requestAnimationFrame(() =>
          document.getElementById(startFieldId)?.focus(),
        );
    });
  }, [choose, compact]);
  // Only release a pending reveal in the exact workspace the host resolved.
  const revealNext = useRef<
    { path: string; host: string; target: FileTarget } | undefined
  >(undefined);
  const cwd = session ? folderOf(session) : "";
  const ownerHost = session?.host || hosts.active;
  useEffect(() => {
    const pending = revealNext.current;
    if (
      !selected ||
      !pending ||
      cwd !== pending.path ||
      ownerHost !== pending.host
    )
      return;
    revealNext.current = undefined;
    openHere(pending.target);
  }, [selected, cwd, ownerHost, openHere]);
  const showInPlace = useCallback(
    (
      place: Pick<LibraryPlace, "path" | "host" | "session">,
      target: FileTarget,
      linked = false,
    ) => {
      if (place.path === cwd && (place.host || "") === ownerHost && selected) {
        setPage("chat");
        openHere(target);
        return;
      }
      // A copied link stays reloadable when its original chat has moved.
      // Navigate with the complete file address in one history transition.
      const followLink = (id: string) => {
        const url = artifactURL(
          id,
          place.host || "",
          target.path,
          target.origin,
        );
        if (url) location.replace(url);
      };
      if (linked && place.session) {
        followLink(place.session);
        return;
      }
      const pending = { path: place.path, host: place.host || "", target };
      revealNext.current = pending;
      setFilesOpen(false);
      if (place.session) void choose(place.session, pending.host);
      else
        void startConversation(place.path, "", "", pending.host)
          .then((id) => {
            if (id && linked && selectedRef.current === id) followLink(id);
            if (!id && revealNext.current === pending)
              revealNext.current = undefined;
          })
          .catch((error) => {
            if (revealNext.current === pending) revealNext.current = undefined;
            report(error);
          });
    },
    [cwd, ownerHost, selected, openHere, choose, startConversation, report],
  );
  const lookup = useRef(0);
  // Catalogue refreshes should not replace every transcript action. Resolve
  // against the latest catalogue when the paired host answers.
  const fileNav = useRef({
    sessions: catalogue.sessions,
    active: hosts.active,
    showInPlace,
  });
  fileNav.current = {
    sessions: catalogue.sessions,
    active: hosts.active,
    showInPlace,
  };
  const openFile = useCallback(
    async (target: FileTarget, linked = false): Promise<void> => {
      const request = ++lookup.current;
      revealNext.current = undefined;
      const id = selectedRef.current;
      if (!id) return;
      try {
        const source = await api<FileOrigin>(routes.fileOrigin(id, target));
        if (id !== selectedRef.current || request !== lookup.current) return;
        const nav = fileNav.current;
        const executionHost = source.host || nav.active;
        const at = nav.sessions.find(
          (s) =>
            s.id !== id &&
            folderOf(s) === source.cwd &&
            (s.host || nav.active) === executionHost,
        );
        const resolved = {
          ...target,
          origin: source.origin,
          ...(target.turn && !source.current && { journal: id }),
        };
        if (source.current && source.cwd === folderRef.current)
          openHere(resolved);
        else
          nav.showInPlace(
            { path: source.cwd, host: executionHost, session: at?.id },
            resolved,
            linked,
          );
      } catch (error) {
        if (id !== selectedRef.current || request !== lookup.current) return;
        setFilesOpen(false);
        const { choices } = failure(error);
        if (!target.origin && choices?.length) {
          open({
            type: "file-origin",
            path: target.path,
            choices,
            linked,
            pick: (origin) => {
              if (id !== selectedRef.current || request !== lookup.current)
                return;
              if (linked) {
                const url = artifactURL(
                  id,
                  fileNav.current.active,
                  target.path,
                  origin,
                );
                if (url) underLayers(() => location.replace(url));
              } else void openFile({ ...target, origin });
            },
          });
        } else report(error);
      }
    },
    [api, routes, openHere, open, report],
  );
  const showLinkedFile = useCallback(
    (path: string, origin?: string) => {
      if (!path) {
        ++lookup.current;
        revealNext.current = undefined;
        setFilesOpen(false);
        return;
      }
      setPage("chat");
      setDrawer(false);
      void openFile({ path, origin, exact: true }, true);
    },
    [openFile],
  );
  useLinkedFile(
    session?.id ?? "",
    !!filesOpen,
    flows[fileScope]?.view,
    showLinkedFile,
  );
  const { submit, deliver, forkAndOpen, showContext, runCommand } = useCommands(
    {
      host,
      session,
      running,
      busy,
      setBusy,
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
      askSide: (parent: Session, question: string) =>
        void side.ask(parent, question).then((id) => {
          if (id && !sideColumn) return choose(id);
        }),
    },
  );
  const chatActions = useMessageActions({
    host,
    act,
    forkAndOpen,
    session,
    deliver,
    busy,
    updateDraft,
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
      side.clearSides();
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
  // This conversation in Settings is the open one: another is opened first.
  const conversationSettings = useCallback(
    (item: Session, address: string) =>
      void (
        item.id === selectedRef.current ? Promise.resolve() : choose(item.id)
      ).then(() => open({ type: "settings", address }), report),
    [choose, open, report],
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
        settings={(address) => conversationSettings(item, address)}
        detail={withDetail ? detailControl : undefined}
        more={extra?.more}
        select={
          withDetail ? undefined : () => latestSelection.current.start(item.id)
        }
      >
        {extra?.top}
      </ConversationMenu>
    ),
    [online, fork, load, report, open, detailControl, conversationSettings],
  );
  const step = (key: "ArrowUp" | "ArrowDown") => {
    const order = sessionOrder(
      rows,
      catalogue.spaces,
      online,
      pins,
      hosts.active,
      hosts.sources.length < 2,
      selected,
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
  const toDraft = useCallback(
    (change: (draft: Draft) => Draft, keep = false) => {
      updateDraft(selected, change);
      if (keep) return;
      if (compact) hideTerminal();
      setFilesOpen((open) => !filesSheet && !!open);
      focusComposer();
    },
    [updateDraft, selected, compact, filesSheet, hideTerminal],
  );
  const addToChat = useCallback(
    (text: string, keep = false) =>
      toDraft(
        (current) => ({
          ...current,
          text: current.text ? `${current.text}\n${text}` : text,
        }),
        keep,
      ),
    [toDraft],
  );
  const addQuote = useCallback(
    (text: string) =>
      toDraft((current) => ({ ...current, quotes: [...current.quotes, text] })),
    [toDraft],
  );
  const drafting = page === "chat" && session && online;
  const toChat = drafting ? addToChat : undefined;
  const quote = drafting ? addQuote : undefined;
  const files = session && (
    <Files
      key={fileScope}
      session={session}
      close={filesSheet ? undefined : () => setFilesOpen(false)}
      expanded={filesOpen === "expanded" && !filesSheet}
      expand={
        filesSheet
          ? undefined
          : () =>
              setFilesOpen((open) => (open === "expanded" ? true : "expanded"))
      }
      flow={flows[fileScope] ?? START}
      setFlow={(update) => setFlow(fileScope, update)}
      addToChat={toChat}
      quote={quote}
      askSide={
        toChat &&
        ((quote) => {
          side.quote(quote);
          setFilesOpen((open) => !filesSheet && !!open);
        })
      }
      copyLink={(link) =>
        void copyText(link).then(() => setNotice("Link copied"), report)
      }
      pictures={browserAvailable}
    />
  );
  const terminal = (
    <Deferred
      load={terminalPanel}
      fallback={<Spinner label="Opening the terminal…" />}
      cwd={session ? folderOf(session) : ""}
      quote={quote}
      shown={!!terminalShown}
      expanded={terminalExpanded}
      expand={
        compact ? undefined : () => setTerminalExpanded(!terminalExpanded)
      }
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
    settings: () => open({ type: "settings" }),
    previous: () => step("ArrowUp"),
    next: () => step("ArrowDown"),
    terminal: terminalAvailable ? toggleTerminal : undefined,
  };
  useShortcuts(authenticated ? shortcutActions : {});
  // #settings=…: opens Settings there, on load and when such a link is
  // entered; entries the app wrote itself (state set) never reopen it.
  useEffect(() => {
    if (!authenticated) return;
    const follow = () => {
      const address = settingsFromURL();
      if (address !== undefined && !history.state)
        setModal({ type: "settings", address, linked: true });
    };
    follow();
    addEventListener("hashchange", follow);
    return () => removeEventListener("hashchange", follow);
  }, [authenticated]);
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
  // Where providers are set up.
  const providers = useCallback(
    () => open({ type: "settings", address: SETTINGS_AT.providers }),
    [open],
  );
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
      uploadBytes: catalogue.upload_bytes,
    }),
    [
      online,
      connection,
      report,
      pins,
      snapshots,
      load,
      refresh,
      forget,
      catalogue.upload_bytes,
    ],
  );
  const conversation = useMemo(
    () => ({
      drafts,
      setDraft,
      submit,
      act,
      upload: attach,
      busy,
      following,
      setFollowing,
    }),
    [drafts, setDraft, submit, act, attach, busy, following, setFollowing],
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
      home={home}
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
        <DetailContext.Provider value={display}>
          <ImageViewer.Provider value={setViewed}>
            <Toasts
              error={error}
              code={errorCode}
              notice={notice}
              dismiss={() => {
                setError("");
                setNotice("");
              }}
              providers={providers}
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
                        edge={false}
                        close={() => setDrawer(false)}
                        actions={
                          <ListActions
                            search={openSearch}
                            home={home}
                            create={newPlace}
                            online={online}
                            brief
                          />
                        }
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
                      sides={
                        page === "chat" &&
                        sideColumn &&
                        (side.list.length > 0 ||
                          side.sides.length > 0 ||
                          !!side.asking)
                          ? {
                              count: side.list.length,
                              shown: side.column,
                              toggle: side.toggle,
                            }
                          : undefined
                      }
                      filesOpen={!!filesOpen}
                      setFilesOpen={setFilesOpen}
                      outgoing={outgoing}
                      parent={sessionById(
                        catalogue.sessions,
                        session?.parent || session?.side?.parent,
                      )}
                      detach={
                        session && isSideChat(session)
                          ? () => void side.detach(session).catch(report)
                          : undefined
                      }
                      openDrawer={openDrawer}
                      choose={choose}
                      settings={settings}
                      menu={conversationMenu}
                    />
                    {filesOpen && filesSheet && page === "chat" && session && (
                      <Modal
                        title="Files"
                        layout="sheet"
                        className="files-sheet"
                        backAddress={withoutFileAddress()}
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
                        hide={() => overview(shell.folder)}
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
                      start={startUnfiled}
                      openList={compact ? openDrawer : undefined}
                      redirect={redirect}
                      updateView={updateView}
                      choose={choose}
                      side={side}
                      sideColumn={sideColumn}
                      quote={quote}
                      actions={chatActions}
                      setInspector={setInspector}
                      files={filesOpen && !filesSheet && files}
                      find={find}
                      closeFind={closeFind}
                    />
                    {terminalStarted && !compact && (
                      <section
                        class="terminal-dock"
                        data-expanded={terminalExpanded || undefined}
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
                    settings={(address: string) =>
                      open({ type: "settings", address })
                    }
                    conversation={!!session}
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
                detail: detailControl,
                library: (kind: LibraryKind) => {
                  setModal(null);
                  setLibraryKind(kind);
                  setPage("library");
                },
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
