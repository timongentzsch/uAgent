import { storage } from "../shared/storage.ts";
import type {
  AppModal,
  JSONValue,
  Draft,
  InstallPrompt,
  Block,
  Session,
  Act,
  CommandKind,
  CommandFields,
  SlashCommand,
} from "../shared/types.ts";
import { failure } from "../shared/types.ts";
import type { JSX } from "preact";
import { render } from "preact";
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import { emptyDraft, hasContent } from "../state/store.ts";
import { api, command, requestId } from "../state/api.ts";
import {
  Modal,
  Deferred,
  IconButton,
  Button,
  Spinner,
  preloadDeferred,
  ErrorBoundary,
} from "../shared/ui.tsx";
import { Ellipsis, Globe2, ListTree, Menu, Settings } from "lucide-preact";
import { StatusLed } from "../shared/connection-status.tsx";
import { ImageViewer, type ViewedImage } from "../shared/attachments.tsx";
// Prefetch helpers live next to the renderer so marker regexes stay in one
// place. Loaded dynamically: a static import would drag markdown.css into
// the initial bundle and break the CSS size budget.
const markdownView = () => import("../shared/markdown-view.tsx");
import { useDismiss } from "../shared/dismiss.ts";
import {
  QUEUED_NEXT,
  queuedGuidance,
  recallable,
  unsent,
} from "../shared/message-view.ts";
import Sidebar, {
  ConversationMenu,
  restartConversation,
  sessionOrder,
  shareTranscript,
} from "../features/sidebar/sidebar.tsx";
import { useShortcuts } from "../shared/shortcuts.ts";
import { nextIndex } from "../shared/listbox-nav.ts";
import { focusDecision, followDecisionLink } from "../shared/navigation.ts";
import Board, { CoordinatorHelp } from "../features/coordinator/board.tsx";
import { threadsOf, waiting as waitingOn } from "../state/attention.ts";
import { folderName } from "../shared/folder-label.tsx";
import {
  lockPageZoom,
  useMedia,
  usePhone,
  trackViewport,
} from "../shared/layout.ts";

import { useHost } from "../state/use-host.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { parseSlash } from "../features/composer/slash.ts";
import { TimePrefsContext } from "../shared/time.ts";
import { DetailContext, detail as detailOf } from "../shared/verbosity.ts";
import type { InspectorTarget } from "../features/chat/inspector.tsx";
import ChatPage, { isBrowsing, transcriptBlocks } from "./chat-page.tsx";
import Modals from "./modals.tsx";
import { usePreferences } from "./use-preferences.ts";
import { useDraftUploads } from "./use-draft-uploads.ts";
import {
  libraryModule,
  pairing,
  instructionsDialog,
  scheduledModule,
  settingsDialog,
  toolsDialog,
  statisticsDialog,
  paletteDialog,
  shortcutsDialog,
} from "./dialogs.ts";

// Own scroll restoration from the first paint. history restoration only
// covers the document; the early opt-out in index.html (<head>, before any
// module) keeps the browser from restoring the inner transcript scroller
// after our mount pin (the mobile reload jump). Repeated here as a
// backstop for mounts that precede it. The transcript hook owns all
// scrolling from here on.
if (typeof history !== "undefined") history.scrollRestoration = "manual";

const BROWSER_KEY = "uagent-browser";

// The shell: navigation, header, toasts and dialogs. It never reads a whole
// snapshot, only what it selects from one (the session's metadata, whether
// its agent browses, whether any turn waits), so a streamed frame re-renders
// the conversation (<ChatPage>), not the shell and its sidebar.
function App() {
  const [page, setPage] = useState<"chat" | "library" | "scheduled">("chat");
  // Which Library tab /memory or /skills asked for.
  const [libraryKind, setLibraryKind] = useState<"memory" | "skills">("memory");
  // Library and Scheduled sit above the conversation: back returns to it.
  useDismiss(page !== "chat", () => setPage("chat"));
  const [drawer, setDrawer] = useState(false);
  const compact = useMedia("(max-width: 900px)");
  // On a phone a coordinator's board slides in from the right on demand
  // instead of taking the top of its chat.
  const phone = usePhone();
  const [boardOpen, setBoardOpen] = useState(false);
  // The drawer belongs to the compact layout; a wider window drops it.
  useEffect(() => setDrawer(false), [compact]);
  const [modal, setModal] = useState<AppModal | null>(null);
  // The command palette (Mod+K) or the shortcuts sheet (?).
  const [overlay, setOverlay] = useState<"palette" | "shortcuts" | null>(null);
  // Remembered per device, so the header's browser slot is right from the
  // first frame instead of appearing once the host answers.
  const [browserAvailable, setBrowserAvailable] = useState(
    () => storage.getItem(BROWSER_KEY) === "1",
  );
  const [notice, setNotice] = useState("");
  // The banner shows one line: a new notice replaces an older error.
  useEffect(() => {
    if (notice) setError("");
  }, [notice]);
  // The one image viewer every tile opens.
  const [viewed, setViewed] = useState<ViewedImage | null>(null);
  const [side, setSide] = useState<{
    question: string;
    answer?: string;
  } | null>(null);
  const closeSide = useCallback(() => setSide(null), []);
  const onResult = useCallback((value: JSONValue, inspect: boolean) => {
    if (inspect) setModal({ type: "raw", value });
    else setNotice(typeof value === "string" ? value : JSON.stringify(value));
  }, []);
  const {
    managementVersion,
    authenticated,
    online,
    connection,
    loadErrors,
    catalogue,
    listed,
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
  } = useHost(onResult, page === "chat");
  useEffect(() => {
    if (authenticated !== true || !online) return;
    let active = true;
    const known = (available: boolean) => {
      if (!active) return;
      setBrowserAvailable(available);
      storage.setItem(BROWSER_KEY, available ? "1" : "0");
    };
    api("/api/browser/status")
      .then(() => known(true))
      .catch(() => known(false));
    return () => {
      active = false;
    };
  }, [authenticated, online]);
  const [folder, setFolder] = useState("");
  const [busy, setBusy] = useState(false);
  const {
    zoom,
    setZoom,
    timePrefs,
    setTimePrefs,
    theme,
    setTheme,
    motion,
    setMotion,
  } = usePreferences();
  const [install, setInstall] = useState<InstallPrompt | null>(null);
  const [update, setUpdate] = useState<ServiceWorker | null>(null);
  const [notificationMode, setNotificationMode] = useState(false);
  // How much of the agent's work shows: one level for every conversation,
  // browser and terminal. Only rendering reads it, so a change restyles
  // every conversation where it stands.
  const detail = useMemo(
    () => detailOf(catalogue.verbosity),
    [catalogue.verbosity],
  );
  const levels = catalogue.verbosity?.order || [];
  const changeDetail = (level: string) => setVerbosity(level).catch(report);
  const [inspector, setInspector] = useState<InspectorTarget | null>(null);
  const metadata = useSnapshots(snapshots, (all) => all[selected]?.metadata);
  const session =
    metadata || catalogue.sessions.find((item) => item.id === selected);
  // Until the catalogue answers, the shell renders with placeholders in the
  // places data will fill; a conversation in the URL keeps its surfaces.
  const booting = authenticated === null;
  const opening = booting && !!selected;
  const draft = drafts[selected] || emptyDraft();
  // Uploads belong to their conversation: one in flight elsewhere never
  // holds this composer.
  const uploading = draft.files.some((file) => file.pending);
  const running = online && !!session?.turn_active;
  // The browser icon shows when this conversation's agent is using it.
  const browsing = useSnapshots(snapshots, (all) => isBrowsing(all[selected]));
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
  const { updateDraft, attachAnnotated, upload } = useDraftUploads({
    session,
    online,
    selected,
    draft,
    uploading,
    setDrafts,
    report,
  });
  function setDraft(value: Draft, id = selected) {
    updateDraft(id, () => value);
  }
  useEffect(() => {
    // The core renderer chunk is needed for every assistant message, so
    // fetch it immediately at boot (not idle: on mobile the idle callback
    // can fire after the first snapshot already mounted, which spreads
    // the plain-to-markdown height wave across the first seconds and
    // fights the bottom pin). Marker-based chunks warm per text below.
    Promise.allSettled([
      markdownView().then((view) => view.prefetchMarkdown()),
    ]);
    // What a click opens, parsed once the shell has settled (the service
    // worker already holds it), so a first open shows content, not a
    // placeholder for its code.
    const warm = setTimeout(
      () =>
        [
          settingsDialog,
          libraryModule,
          scheduledModule,
          statisticsDialog,
          toolsDialog,
          instructionsDialog,
        ].forEach((load) => preloadDeferred<never>(load).catch(() => {})),
      2000,
    );
    return () => clearTimeout(warm);
  }, []);
  useEffect(trackViewport, []);
  // Files dropped anywhere attach to the open conversation; unhandled, the
  // browser would open the file in place of the app. File inputs and
  // dialogs keep their own drops.
  const drops = useRef({ upload, page });
  drops.current = { upload, page };
  useEffect(() => {
    const files = (event: DragEvent) =>
      event.dataTransfer?.types.includes("Files") &&
      !(
        event.target instanceof HTMLInputElement && event.target.type === "file"
      );
    const over = (event: DragEvent) => files(event) && event.preventDefault();
    const drop = (event: DragEvent) => {
      if (!files(event)) return;
      event.preventDefault();
      if (
        drops.current.page === "chat" &&
        !(event.target as Element).closest?.("dialog")
      )
        void drops.current.upload([...(event.dataTransfer?.files || [])]);
    };
    addEventListener("dragover", over);
    addEventListener("drop", drop);
    return () => {
      removeEventListener("dragover", over);
      removeEventListener("drop", drop);
    };
  }, []);
  useEffect(() => {
    let stop: (() => void) | undefined;
    import("../shared/pwa.ts").then(
      ({ watchPwa }) => (stop = watchPwa(setInstall, setUpdate, report)),
    );
    return () => stop?.();
  }, [report]);

  const act: Act = useCallback(
    async <K extends CommandKind>(kind: K, fields: CommandFields = {}) => {
      if (!online)
        throw new Error("Reconnect and refresh before sending commands.");
      if (fields.request_id) correlate(fields.request_id);
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
  useEffect(() => {
    const openSession = (event: MessageEvent) => {
      if (
        event.data?.type === "OPEN_SESSION" &&
        /^[a-f0-9]{16,64}$/.test(event.data.id || "")
      ) {
        choose(event.data.id);
        if (event.data.decision !== undefined) focusDecision();
      }
    };
    // A notification's link opened this window at a decision.
    followDecisionLink();
    addEventListener("hashchange", followDecisionLink);
    navigator.serviceWorker?.addEventListener("message", openSession);
    return () => {
      navigator.serviceWorker?.removeEventListener("message", openSession);
      removeEventListener("hashchange", followDecisionLink);
    };
  }, []);
  // A conversation in the folder typed into the dialog, or its coordinator.
  async function create(
    event: JSX.TargetedSubmitEvent<HTMLFormElement> | null,
    coordinator = false,
  ) {
    event?.preventDefault();
    setBusy(true);
    try {
      await startConversation(folder, coordinator);
      setModal(null);
    } catch (failure) {
      report(failure);
    } finally {
      setBusy(false);
    }
  }
  // The host returns a folder's existing coordinator rather than a second.
  // One start at a time: a double click must not race two activations.
  const starting = useRef(false);
  const startConversation = useCallback(
    async (cwd: string, coordinator = false) => {
      if (starting.current) return;
      starting.current = true;
      try {
        const created = await command("create", null, { cwd, coordinator });
        if (created.pending) return "";
        upsertSession(created.session);
        // Live before it is shown, so opening never flashes the saved state;
        // shown even if activating fails, with the failure reported.
        try {
          if (created.session.presence !== "active")
            await command("activate", created.session);
        } finally {
          await choose(created.session.id);
        }
        await load(created.session.id);
        return created.session.id;
      } finally {
        starting.current = false;
      }
    },
    [upsertSession, choose, load],
  );
  // A command with a screen opens it when typed bare; with an argument it
  // runs on the host, as in the terminal.
  const screens: Record<string, () => void> = {
    "/context": () => showContext(),
    "/config": () => open({ type: "settings" }),
    "/verbosity": () => open({ type: "settings", section: "user" }),
    "/permissions": () => open({ type: "settings", section: "user" }),
    "/mcp": () => open({ type: "settings", section: "user" }),
    "/tools": () => setModal({ type: "tools", session_id: selected }),
    "/rename": () => session && setModal({ type: "rename", session }),
    "/memory": () => showLibrary("memory"),
    "/skills": () => showLibrary("skills"),
    "/schedule": () => setPage("scheduled"),
    // The list is always beside a wide conversation: find in it instead.
    "/sessions": () =>
      compact
        ? setDrawer(true)
        : document.getElementById("session-search")?.focus(),
  };
  function showLibrary(kind: "memory" | "skills") {
    setLibraryKind(kind);
    setPage("library");
  }
  // A fork opens as its own conversation; the original stays as it was.
  // Editing (a rewind) also puts the message it forked before back into
  // the composer.
  async function forkAndOpen(
    target: Session | undefined,
    fields: CommandFields = {},
    edit = false,
  ) {
    const result = await command("fork", target, fields);
    if (result.pending) return;
    const { id, prompt, rewound } = result.result;
    if (!rewound) {
      await refresh();
      await choose(id);
      await command("activate", { id, generation: "" });
    }
    await load(id);
    if (edit && prompt) setDraft({ text: prompt, files: [] }, id);
    if (edit)
      setNotice(
        rewound
          ? "The coordinator rewound to this message; edit and send."
          : "Continuing in a fork. Files on disk are unchanged.",
      );
  }
  async function localCommand(text: string) {
    const { name, argument } = parseSlash(catalogue.commands || [], text);
    if (!argument && screens[name]) screens[name]();
    else if (name === "/reset") await startConversation(session!.cwd!);
    else if (name === "/verbosity") {
      if (!levels.includes(argument))
        throw new Error(`Use /verbosity ${levels.join("|")}`);
      await changeDetail(argument);
    }
    // In a terminal /quit detaches and the runtime stays; here that is just
    // leaving the page, so it never closes anything.
    else if (name === "/quit")
      throw new Error("Use Close conversation in the conversation menu.");
    else if (name === "/restart") {
      setNotice(await restartConversation(session!));
    } else if (name === "/fork") {
      await forkAndOpen(session, { argument });
    } else if (name === "/rewind" && argument) {
      // Bare /rewind runs on the host and lists the message numbers.
      await forkAndOpen(session, { argument }, true);
    } else if (name === "/btw") {
      if (!argument) throw new Error("Use /btw QUESTION");
      // The card shows the question; the composer is free again at once.
      setSide({ question: argument });
      act("side", { text: argument }).then(
        (result) => {
          if (!result.pending)
            setSide((current) =>
              current?.question === argument
                ? { question: argument, answer: result.result.answer }
                : current,
            );
        },
        (failure) => {
          setSide(null);
          report(failure);
        },
      );
    } else if (name === "/share") {
      setNotice(await shareTranscript(session!));
    } else if (name === "/instructions" && !argument) {
      setModal({ type: "instructions" });
    } else if (name === "/http") {
      const exchanges = snapshots.get()[selected]?.state?.http || [];
      const [number, part = "request"] = argument.split(/\s+/);
      const index = number ? Number(number) : exchanges.length;
      if (
        !Number.isInteger(index) ||
        index < 1 ||
        index > exchanges.length ||
        !["request", "response"].includes(part)
      )
        throw new Error(
          exchanges.length
            ? "Use /http INDEX request|response"
            : "No HTTP exchange captured",
        );
      setModal({
        type: "raw",
        session: selected,
        exchanges: [exchanges[index - 1]],
        part: part as "request" | "response",
      });
    } else return false;
    return true;
  }
  async function submit(event: Event, jumpToLatest: () => void, queue = false) {
    event.preventDefault();
    const pending = snapshots.get()[selected]?.pending;
    setNotice("");
    if (
      !online ||
      pending ||
      (!draft.text.trim() && !draft.files.length) ||
      uploading ||
      busy
    )
      return;
    setBusy(true);
    // Sending always produces bottom content (optimistic message, command
    // output, streamed answer): re-stick synchronously so the turn stays
    // attached even if the stick was lost while reading history or the
    // send-frame layout churn (composer shrink, optimistic append) moves
    // scroll before the observers re-pin. Idempotent when already sticky.
    jumpToLatest();
    const id = selected,
      sent = draft,
      request_id = requestId();
    if (sent.text.startsWith("/")) {
      try {
        // A side question is the one command meant for a running turn.
        if (running && !sent.text.startsWith("/btw "))
          throw new Error(
            "Wait for this turn to finish before running a slash command.",
          );
        if (await localCommand(sent.text)) {
          setDrafts((current) =>
            current[id] === sent
              ? { ...current, [id]: { ...sent, text: "" } }
              : current,
          );
          setBusy(false);
          return;
        }
        if (
          parseSlash(catalogue.commands || [], sent.text).name === "/attach" &&
          sent.text.trim().endsWith(" clear")
        )
          updateDraft(id, emptyDraft);
      } catch (error) {
        report(error);
        setBusy(false);
        return;
      }
    }
    await deliver(id, sent, queue, request_id);
  }
  // A message goes out as a row of its own until the host confirms it; a
  // failed one stays there, with Retry and Return to composer. Guidance
  // joins the running turn, or with `queue` waits for it to end.
  async function deliver(
    id: string,
    sent: Draft,
    queue = false,
    request_id = requestId(),
  ) {
    setBusy(true);
    const kind = running && !snapshots.get()[id]?.pending ? "steer" : "submit";
    const queued =
      kind === "steer" && (queue ? QUEUED_NEXT : "Guidance queued");
    const visible = !sent.text.startsWith("/") || sent.files.length;
    if (visible)
      setOutgoing((items) => [
        ...items,
        {
          id: `outgoing-${request_id}`,
          request_id,
          session_id: id,
          kind: "user",
          text: sent.text,
          files: sent.files,
          time: new Date().toISOString(),
          status: queued || "Sending…",
        },
      ]);
    setDrafts((current) =>
      current[id] === sent ? { ...current, [id]: emptyDraft() } : current,
    );
    try {
      const result = await act(kind, {
        request_id,
        text: sent.text,
        ...(queued && queue && { queue }),
        attachment_ids: sent.files.map((item) => ({
          id: item.id,
          name: item.name,
        })),
      });
      patchOutgoing(request_id, {
        status: result.pending ? "Awaiting confirmation" : queued || "Sent",
      });
    } catch (error) {
      const issue = failure(error);
      if (visible)
        patchOutgoing(request_id, {
          status: issue.rejected ? "Not sent" : "Not confirmed",
          error: issue.message,
        });
      else report(issue);
    } finally {
      setBusy(false);
    }
  }
  // Every row reads these through one context value, so they must stay the
  // same function; they read the latest state through a ref.
  const current = {
    online,
    selected,
    act,
    report,
    forkAndOpen,
    outgoing,
    session,
    deliver,
    busy,
    showContext,
    sessions: catalogue.sessions,
  };
  const latest = useRef(current);
  latest.current = current;
  // A failed message sends again as a new request; Continue resumes a
  // stopped turn. Both leave the composer's draft alone.
  const retrySend = useCallback((block: Block) => {
    const { online, selected, deliver, busy } = latest.current;
    if (!online || busy || !block.request_id || !unsent(block)) return;
    setOutgoing((items) =>
      items.filter((item) => item.request_id !== block.request_id),
    );
    void deliver(selected, {
      text: block.text || "",
      files: (block.files || []).filter((file) => typeof file === "object"),
    });
  }, []);
  const resume = useCallback(() => {
    const { online, selected, deliver, busy } = latest.current;
    if (online && !busy)
      void deliver(selected, { text: "continue", files: [] });
  }, []);
  // From a message's menu: edit it in a fork (fork before it), or keep it
  // and its reply (fork before the next message of yours).
  const branchFrom = useCallback((block: Block, edit: boolean) => {
    const { forkAndOpen, outgoing, selected, report } = latest.current;
    const blocks = transcriptBlocks(
      snapshots.get()[selected]?.state?.view,
      outgoing,
      selected,
    );
    const after = blocks.slice(
      blocks.findIndex((item) => item.id === block.id) + 1,
    );
    const next = after.find((item) => item.kind === "user");
    forkAndOpen(
      latest.current.session,
      edit ? { message_id: block.id } : next ? { message_id: next.id } : {},
      edit,
    ).catch(report);
  }, []);
  // Recall returns queued guidance to the composer while it is still
  // queued. Delivered guidance belongs to the turn; dropping the row is
  // then the only correct move. A send that failed returns the same way,
  // with nothing to withdraw from the host.
  const recallGuidance = useCallback(async (block: Block) => {
    const { online, selected, act, report } = latest.current;
    const target = block.request_id;
    if (!target || !recallable(block, online)) return;
    const queued = queuedGuidance(block);
    const text = block.text || "";
    const id = selected;
    if (queued) {
      try {
        await act("recall", { target_id: target });
      } catch (error) {
        const issue = failure(error);
        if (!/already delivered/i.test(issue.message)) {
          report(error);
          return;
        }
      }
    }
    setOutgoing((items) => items.filter((item) => item.request_id !== target));
    if (text)
      updateDraft(id, (draft) => ({
        ...draft,
        text: draft.text ? `${draft.text}\n${text}` : text,
      }));
  }, []);
  const messageActions = useMemo(
    () => ({
      report,
      recall: recallGuidance,
      retry: retrySend,
      resume,
      branch: branchFrom,
      inspect: (id: string) =>
        setInspector({
          title: id.startsWith("t-") ? "Tool input/output" : "Full content",
          raw: { id, session: selected },
        }),
      http: (exchanges: NonNullable<Block["http"]>) =>
        setModal({ type: "raw", session: selected, exchanges }),
      activity: (block: Block) => setInspector({ block }),
      statistics: (block: Block) =>
        setModal({
          type: "statistics",
          session_id: selected,
          block_id: block.occurrence_id || block.response_id || block.id,
          unit: block.summary ? "Turn" : "Message",
        }),
    }),
    [report, recallGuidance, branchFrom, selected],
  );
  // The other conversations of the open one's folder. A coordinator's chat
  // names who of them wrote what and shows each one's prompt.
  const threads = useMemo(
    () => threadsOf(catalogue.sessions, session?.cwd || ""),
    [catalogue.sessions, session?.cwd],
  );
  const teamNames =
    session?.kind === "coordinator"
      ? threads
          .map((item) => item.member)
          .filter(Boolean)
          .join(" ")
      : "";
  const chatActions = useMemo(
    () =>
      teamNames
        ? {
            ...messageActions,
            team: threadsOf(
              latest.current.sessions,
              latest.current.session?.cwd || "",
            ).filter((item) => item.member),
            prompt: (member?: string) => latest.current.showContext(member),
          }
        : messageActions,
    [messageActions, teamNames],
  );
  async function logout() {
    try {
      const registration = await navigator.serviceWorker?.getRegistration();
      await (await registration?.pushManager?.getSubscription())?.unsubscribe();
      await command("logout");
      reset();
      setModal(null);
    } catch (failure) {
      report(failure);
    }
  }
  // The context a session was last sent, its system prompt first: the open
  // conversation's, or that of a member of its chat.
  function showContext(member?: string) {
    const shown = member
      ? threads.find((item) => item.member === member)
      : session;
    const busy = member ? shown?.turn_active : running;
    const prepare = shown?.generation && !busy && online ? shown : undefined;
    const id = shown?.id || selected;
    setModal({
      type: "raw",
      context: true,
      session: id,
      exchanges: prepare ? [] : snapshots.get()[id]?.state?.http || [],
      prepare,
    });
  }

  const open = useCallback((value: AppModal) => {
    setDrawer(false);
    setModal(value);
  }, []);
  // Forks any conversation from its menu, as /fork does the open one.
  const fork = useCallback(
    (item: Session) => latest.current.forkAndOpen(item).catch(report),
    [report],
  );
  // The open conversation's menu also holds the level its transcript shows.
  const conversationMenu = useCallback(
    (item: Session, withDetail = false) => (
      <ConversationMenu
        item={item}
        online={online}
        fork={fork}
        loadSnapshot={load}
        report={report}
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
      />
    ),
    // levels and changeDetail follow the catalogue.
    [online, fork, load, report, open, detail, catalogue.verbosity],
  );
  // A slash command from the palette runs as if sent from the composer;
  // one that needs an argument waits there for it.
  async function runCommand(entry: SlashCommand) {
    try {
      if (entry.argument && !entry.argument.startsWith("[")) {
        if (!session)
          throw new Error(`Open a conversation to use ${entry.command}`);
        setDraft({ ...draft, text: `${entry.command} ` });
        requestAnimationFrame(() => document.getElementById("prompt")?.focus());
      } else if (!(await localCommand(entry.command))) {
        if (!session)
          throw new Error(`Open a conversation to run ${entry.command}`);
        if (running)
          throw new Error(
            "Wait for this turn to finish before running a slash command.",
          );
        await act("submit", { request_id: requestId(), text: entry.command });
      }
    } catch (failure) {
      report(failure);
    }
  }
  // Alt+↑/↓: the conversation above or below in the list.
  const step = (key: "ArrowUp" | "ArrowDown") => {
    const order = sessionOrder(listed);
    if (!order.length) return;
    const at = order.findIndex((item) => item.id === selected);
    choose(order[nextIndex(at, key, order.length)].id);
  };
  const shortcutActions = {
    palette: () => setOverlay("palette"),
    shortcuts: () => setOverlay("shortcuts"),
    previous: () => step("ArrowUp"),
    next: () => step("ArrowDown"),
  };
  // Only for a paired device: the pairing screen has nothing to find.
  useShortcuts(authenticated ? shortcutActions : {});
  const openPalette = useCallback(() => {
    setDrawer(false);
    setOverlay("palette");
  }, []);
  const navigate = useCallback((value: typeof page) => {
    setPage(value);
    setDrawer(false);
  }, []);
  const settings = useCallback(() => open({ type: "settings" }), [open]);
  const cwd = session?.cwd || "";
  const newConversation = useCallback(() => {
    setFolder(cwd);
    open({ type: "new" });
  }, [cwd, open]);
  const coordinate = useCallback(
    (cwd: string) => startConversation(cwd, true).catch(report),
    [startConversation, report],
  );
  const projects = [
    ...new Set(
      catalogue.sessions
        .map((entry) => entry.cwd)
        .filter((path): path is string => !!path),
    ),
  ];
  const sidebar = (
    <Sidebar
      loading={booting}
      page={page}
      navigate={navigate}
      scheduledUnread={
        !!catalogue.scheduled?.runs?.some((run) => unread.has(run.session_id))
      }
      sessions={listed}
      selected={page === "chat" ? selected : ""}
      unread={unread}
      online={online}
      connection={connection}
      choose={choose}
      menu={conversationMenu}
      refresh={refresh}
      settings={settings}
      create={newConversation}
      coordinate={coordinate}
      palette={openPalette}
      report={report}
    />
  );

  return (
    <TimePrefsContext.Provider value={timePrefs}>
      <DetailContext.Provider value={detail}>
        <ImageViewer.Provider value={setViewed}>
          {/* Notices float over the app, never push it down. */}
          <div class="toasts">
            {(error || notice) && (
              <div role={error ? "alert" : "status"} class="error-banner">
                <span>{error || notice}</span>
                <Button
                  onClick={() => {
                    setError("");
                    setNotice("");
                  }}
                >
                  Dismiss
                </Button>
              </div>
            )}
            {/* A waiting service worker means this view is stale (notably in an
          installed PWA with no chrome to pull-to-refresh). Surface it
          here, not only in Settings, so reloads actually pick up fixes. */}
            {update && (
              <div role="status" class="update-banner">
                <span>Update available with the latest fixes.</span>
                <Button
                  variant="primary"
                  disabled={!!updateBlocked}
                  title={
                    updateBlocked || "Reloads this view with the latest fixes."
                  }
                  onClick={() =>
                    import("../shared/pwa.ts").then(({ applyUpdate }) =>
                      applyUpdate(update),
                    )
                  }
                >
                  Refresh now
                </Button>
              </div>
            )}
          </div>
          {authenticated === false ? (
            <Deferred
              load={pairing}
              paired={refresh}
              report={report}
              fallback={<Spinner label="Loading connection form…" surface />}
            />
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
                    aria-label="Projects and conversations"
                  >
                    {sidebar}
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
                  <header class="conversation-head">
                    {compact && (
                      <IconButton
                        label="Open conversations"
                        onClick={() => setDrawer(true)}
                      >
                        <Menu />
                      </IconButton>
                    )}
                    <div>
                      <h1 title={session?.cwd}>
                        {page === "library" ? (
                          "Library"
                        ) : page === "scheduled" ? (
                          "Scheduled"
                        ) : session?.kind === "coordinator" ? (
                          `Coordinator · ${folderName(session.cwd)}`
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
                      {page === "chat" && session?.kind === "coordinator" && (
                        <CoordinatorHelp
                          editInstructions={() =>
                            open({ type: "instructions" })
                          }
                        />
                      )}
                    </div>
                    {browserAvailable && (
                      <IconButton
                        label={
                          browsing
                            ? "Open browser, agent working"
                            : "Open browser"
                        }
                        class="browser-toggle"
                        disabled={booting}
                        onClick={() => setModal({ type: "browser" })}
                      >
                        <Globe2 />
                        {browsing && <StatusLed state="running" />}
                      </IconButton>
                    )}
                    {phone &&
                      page === "chat" &&
                      session?.kind === "coordinator" &&
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
                        conversationMenu(session, true)
                      ) : opening ? (
                        // Inert until the session is known.
                        <IconButton label="Conversation menu" disabled>
                          <Ellipsis />
                        </IconButton>
                      ) : null)}
                  </header>
                  {boardOpen && phone && session?.kind === "coordinator" && (
                    <Modal
                      title="Board"
                      layout="sheet"
                      className="side-sheet"
                      close={() => setBoardOpen(false)}
                    >
                      <Board
                        threads={threads}
                        online={online}
                        choose={(id) => {
                          setBoardOpen(false);
                          void choose(id);
                        }}
                      />
                    </Modal>
                  )}
                  {page !== "chat" && (
                    <Deferred
                      key={page === "library" ? `library:${libraryKind}` : page}
                      load={
                        page === "library" ? libraryModule : scheduledModule
                      }
                      initialKind={libraryKind}
                      projects={projects}
                      cwd={session?.cwd || projects[0] || ""}
                      online={online}
                      version={managementVersion}
                      scheduled={catalogue.scheduled}
                      unread={unread}
                      choose={choose}
                      refresh={refresh}
                      fallback={
                        <div class="management">
                          <Spinner
                            label={
                              page === "library"
                                ? "Loading library…"
                                : "Loading scheduled tasks…"
                            }
                            surface
                          />
                        </div>
                      }
                    />
                  )}
                  <ChatPage
                    store={snapshots}
                    active={page === "chat"}
                    historyKey={
                      page === "chat" ? `${page}:${selected}` : `page:${page}`
                    }
                    selected={selected}
                    session={session}
                    opening={opening}
                    catalogue={catalogue}
                    online={online}
                    connection={connection}
                    outgoing={outgoing}
                    loadError={loadErrors[selected]}
                    draft={draft}
                    setDraft={setDraft}
                    upload={upload}
                    uploading={uploading}
                    busy={busy}
                    submit={submit}
                    act={act}
                    report={report}
                    following={following}
                    setFollowing={setFollowing}
                    load={load}
                    updateView={updateView}
                    choose={choose}
                    zoom={zoom}
                    side={side}
                    closeSide={closeSide}
                    actions={chatActions}
                    setModal={setModal}
                    setInspector={setInspector}
                    showContext={() => showContext()}
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
              title={
                overlay === "palette" ? "Command palette" : "Keyboard shortcuts"
              }
              layout="sheet"
              size="narrow"
              close={() => setOverlay(null)}
            >
              {overlay === "palette" ? (
                <Deferred
                  load={paletteDialog}
                  fallback={<Spinner label="Loading…" surface />}
                  sessions={listed}
                  commands={catalogue.commands || []}
                  choose={choose}
                  start={(cwd: string, coordinator?: boolean) =>
                    void startConversation(cwd, coordinator).catch(report)
                  }
                  run={runCommand}
                  settings={(section: string) =>
                    open({ type: "settings", section })
                  }
                  actions={{ ...shortcutActions, palette: undefined }}
                />
              ) : (
                <Deferred
                  load={shortcutsDialog}
                  fallback={<Spinner label="Loading…" surface />}
                />
              )}
            </Modal>
          )}
          <Modals
            store={snapshots}
            modal={modal}
            setModal={setModal}
            inspector={inspector}
            setInspector={setInspector}
            viewed={viewed}
            setViewed={setViewed}
            annotate={session && online ? attachAnnotated : undefined}
            selected={selected}
            session={session}
            catalogue={catalogue}
            online={online}
            compact={compact}
            load={load}
            refresh={refresh}
            forget={forget}
            report={report}
            managementVersion={managementVersion}
            projects={projects}
            showContext={() => showContext()}
            folder={folder}
            setFolder={setFolder}
            create={create}
            busy={busy}
            preferences={{
              theme,
              setTheme,
              motion,
              setMotion,
              timePrefs,
              setTimePrefs,
              zoom,
              setZoom,
              install,
              setInstall,
              update,
              updateBlocked: !!updateBlocked,
              notificationMode,
              setNotificationMode,
              notifications,
              logout,
            }}
          />
        </ImageViewer.Provider>
      </DetailContext.Provider>
    </TimePrefsContext.Provider>
  );
}
lockPageZoom();
render(
  <ErrorBoundary>
    <App />
  </ErrorBoundary>,
  document.getElementById("app")!,
);
