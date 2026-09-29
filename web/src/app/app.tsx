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
import { readStored, writeStored } from "../state/store.ts";
import { api, command, requestId, uploadAttachment } from "../state/api.ts";
import {
  Modal,
  Deferred,
  IconButton,
  Button,
  Spinner,
  preloadDeferred,
  ErrorBoundary,
} from "../shared/ui.tsx";
import { Ellipsis, Globe2, Menu, Settings } from "lucide-preact";
import { StatusLed } from "../shared/connection-status.tsx";
import { ImageViewer, type ViewedImage } from "../shared/attachments.tsx";
// Prefetch helpers live next to the renderer so marker regexes stay in one
// place. Loaded dynamically: a static import would drag markdown.css into
// the initial bundle and break the CSS size budget.
const markdownView = () => import("../shared/markdown-view.tsx");
import { useDismiss } from "../shared/dismiss.ts";
import { unsent } from "../shared/message-view.ts";
import Sidebar, { ConversationMenu } from "../features/sidebar/sidebar.tsx";
import { CoordinatorHelp } from "../features/coordinator/board.tsx";
import { folderName } from "../shared/folder-label.tsx";
import {
  applyTheme,
  applyZoom,
  lockPageZoom,
  normalizeZoom,
  useMedia,
  trackViewport,
} from "../shared/layout.ts";

import { useHost } from "../state/use-host.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { parseSlash } from "../features/composer/slash.ts";
import { dedupeName } from "../features/composer/mention.ts";
import {
  TimePrefsContext,
  normalizeTimePrefs,
  type TimePrefs,
} from "../shared/time.ts";
import type { InspectorTarget } from "../features/chat/inspector.tsx";
import { maxDraftFiles, maxUploadBytes } from "../shared/limits.ts";
import ChatPage, { isBrowsing, transcriptBlocks } from "./chat-page.tsx";
import Modals from "./modals.tsx";
import {
  libraryModule,
  pairing,
  instructionsDialog,
  scheduledModule,
  settingsDialog,
  toolsDialog,
  statisticsDialog,
} from "./dialogs.ts";

// Own scroll restoration from the first paint. history restoration only
// covers the document; the early opt-out in index.html (<head>, before any
// module) keeps the browser from restoring the inner transcript scroller
// after our mount pin (the mobile reload jump). Repeated here as a
// backstop for mounts that precede it. The transcript hook owns all
// scrolling from here on.
if (typeof history !== "undefined") history.scrollRestoration = "manual";

const emptyDraft = (): Draft => ({ text: "", files: [] });

const BROWSER_KEY = "uagent-browser";

// The conversation menu's button, inert, until the session is known.
function MenuPlaceholder() {
  return (
    <IconButton label="Conversation menu" disabled>
      <Ellipsis />
    </IconButton>
  );
}

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
  // The drawer belongs to the compact layout; a wider window drops it.
  useEffect(() => setDrawer(false), [compact]);
  const [modal, setModal] = useState<AppModal | null>(null);
  // Remembered per device, so the header's browser slot is right from the
  // first frame instead of appearing once the host answers.
  const [browserAvailable, setBrowserAvailable] = useState(
    () => storage.getItem(BROWSER_KEY) === "1",
  );
  const [notice, setNotice] = useState("");
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
    setCatalogue,
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
  const [zoom, setZoom] = useState(() =>
    normalizeZoom(readStored<number>(storage, "uagent-zoom", 100)),
  );
  const [install, setInstall] = useState<InstallPrompt | null>(null);
  const [update, setUpdate] = useState<ServiceWorker | null>(null);
  const [notificationMode, setNotificationMode] = useState(false);
  const [timePrefs, setTimePrefs] = useState<TimePrefs>(() =>
    normalizeTimePrefs(readStored(storage, "uagent-time", {})),
  );
  useEffect(() => writeStored(storage, "uagent-time", timePrefs), [timePrefs]);
  const [theme, setTheme] = useState(
    () => storage.getItem("uagent-theme") || "system",
  );
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
  const showMessageHttp = useCallback(
    (exchanges: NonNullable<Block["http"]>) =>
      setModal({ type: "raw", session: selected, exchanges }),
    [selected],
  );
  const showMessageStatistics = useCallback(
    (block: Block) =>
      setModal({
        type: "statistics",
        session_id: selected,
        block_id: block.occurrence_id || block.response_id || block.id,
        unit: block.summary ? "Turn" : "Message",
      }),
    [selected],
  );
  // The browser icon shows when this conversation's agent is using it.
  const browsing = useSnapshots(snapshots, (all) => isBrowsing(all[selected]));
  // The update banner holds its reload while a turn waits on a decision.
  const waiting = useSnapshots(
    snapshots,
    (all) => !!update && Object.values(all).some((item) => item.pending),
  );
  function setDraft(value: Draft, id = selected) {
    setDrafts((current) => ({ ...current, [id]: value }));
  }
  useEffect(() => {
    writeStored(storage, "uagent-zoom", zoom);
    applyZoom(zoom);
  }, [zoom]);
  // The shared transcript controller restores a returning conversation.
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
  useEffect(() => {
    return trackViewport();
  }, []);
  useEffect(() => applyTheme(theme), [theme]);
  // Files dropped anywhere attach to the open conversation; unhandled, the
  // browser would open the file in place of the app. File inputs and
  // dialogs keep their own drops.
  const dropRef = useRef(upload);
  dropRef.current = upload;
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
      if (!(event.target as Element).closest?.("dialog"))
        void dropRef.current([...(event.dataTransfer?.files || [])]);
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
      )
        choose(event.data.id);
    };
    navigator.serviceWorker?.addEventListener("message", openSession);
    return () =>
      navigator.serviceWorker?.removeEventListener("message", openSession);
  }, []);
  async function create(event: JSX.TargetedSubmitEvent<HTMLFormElement>) {
    event.preventDefault();
    setBusy(true);
    try {
      await startConversation(folder);
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
        if (created.pending) return;
        setCatalogue((prior) => ({
          ...prior,
          sessions: [
            created.session,
            ...prior.sessions.filter(
              (entry) => entry.id !== created.session.id,
            ),
          ],
        }));
        // Live before it is shown, so opening never flashes the saved state;
        // shown even if activating fails, with the failure reported.
        try {
          if (created.session.presence !== "active")
            await command("activate", created.session);
        } finally {
          await choose(created.session.id);
        }
        await load(created.session.id);
      } finally {
        starting.current = false;
      }
    },
    [setCatalogue, choose, load],
  );
  // A command with a screen opens it when typed bare; with an argument it
  // runs on the host, as in the terminal.
  const screens: Record<string, () => void> = {
    "/context": showContext,
    "/config": () => open({ type: "settings" }),
    "/permissions": () => open({ type: "settings", section: "permissions" }),
    "/mcp": () => open({ type: "settings", section: "mcp" }),
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
  async function forkAndOpen(fields: CommandFields, edit = false) {
    const result = await command("fork", session, fields);
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
    else if (name === "/quit") {
      await act("close");
      await load(selected);
    } else if (name === "/fork") {
      await forkAndOpen({ argument });
    } else if (name === "/rewind" && argument) {
      // Bare /rewind runs on the host and lists the message numbers.
      await forkAndOpen({ argument }, true);
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
      const result = await act("share");
      if (!result.pending)
        setNotice(`Transcript saved to ${result.result.path}`);
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
  async function submit(event: Event, jumpToLatest: () => void) {
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
          setDrafts((current) => ({ ...current, [id]: emptyDraft() }));
      } catch (error) {
        report(error);
        setBusy(false);
        return;
      }
    }
    const kind = running && !pending ? "steer" : "submit";
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
          status: kind === "steer" ? "Guidance queued" : "Sending…",
        },
      ]);
    setDrafts((current) =>
      current[id] === sent ? { ...current, [id]: emptyDraft() } : current,
    );
    try {
      const result = await act(kind, {
        request_id,
        text: sent.text,
        ...(kind === "submit" || kind === "steer"
          ? {
              attachment_ids: sent.files.map((item) => ({
                id: item.id,
                name: item.name,
              })),
            }
          : {}),
      });
      setOutgoing((items) =>
        items.map((item) =>
          item.request_id === request_id
            ? {
                ...item,
                status: result.pending
                  ? "Awaiting confirmation"
                  : kind === "steer"
                    ? "Guidance queued"
                    : "Sent",
              }
            : item,
        ),
      );
    } catch (error) {
      const issue = failure(error);
      if (visible)
        setOutgoing((items) =>
          items.map((item) =>
            item.request_id === request_id
              ? {
                  ...item,
                  status: issue.rejected ? "Not sent" : "Not confirmed",
                  error: issue.message,
                }
              : item,
          ),
        );
      else report(issue);
      if (issue.rejected)
        setDrafts((current) =>
          !current[id]?.text && !current[id]?.files?.length
            ? { ...current, [id]: sent }
            : current,
        );
    } finally {
      setBusy(false);
    }
  }
  // Recall returns queued guidance to the composer while it is still
  // queued. Delivered guidance belongs to the turn; dropping the row is
  // then the only correct move. A send that failed returns the same way,
  // with nothing to withdraw from the host.
  // Every row reads these through one context value, so they must stay the
  // same function; they read the latest state through a ref.
  const latest = useRef({
    online,
    selected,
    act,
    report,
    forkAndOpen,
    outgoing,
  });
  latest.current = { online, selected, act, report, forkAndOpen, outgoing };
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
      edit ? { message_id: block.id } : next ? { message_id: next.id } : {},
      edit,
    ).catch(report);
  }, []);
  const openActivity = useCallback(
    (block: Block) => setInspector({ block }),
    [],
  );
  const recallGuidance = useCallback(async (block: Block) => {
    const { online, selected, act, report } = latest.current;
    const target = block.request_id;
    const queued = block.status === "Guidance queued";
    if (!target || !(queued || unsent(block)) || (queued && !online)) return;
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
    if (text) {
      setDrafts((current) => {
        const prior = current[id]?.text || "";
        const next = prior ? `${prior}\n${text}` : text;
        return {
          ...current,
          [id]: { ...(current[id] || emptyDraft()), text: next },
        };
      });
    }
  }, []);
  // An annotated copy joins the draft and replaces the draft file it was
  // drawn on; a copy of a sent image is simply attached.
  async function attachAnnotated(file: File, replaces?: string) {
    const id = selected;
    if (!(await upload([file]))) return false;
    if (replaces)
      setDrafts((current) => {
        const draft = current[id] || emptyDraft();
        return {
          ...current,
          [id]: {
            ...draft,
            files: draft.files.filter((item) => item.id !== replaces),
          },
        };
      });
    return true;
  }
  // Resolves true once every file is attached to the draft.
  async function upload(files: File[]) {
    if (!session || !online || uploading || !files.length) return false;
    const id = selected;
    if (
      files.length + draft.files.length > maxDraftFiles ||
      files.some((file) => file.size > maxUploadBytes)
    ) {
      report(
        new Error(
          `Attach up to ${maxDraftFiles} files, at most ${maxUploadBytes / 1024 / 1024} MiB each.`,
        ),
      );
      return false;
    }
    // Display names dedupe against the live draft, so two pastes never
    // share a label. The deduped name is the upload name: server record,
    // model payload and UI agree with no migration.
    const taken = new Set(draft.files.map((item) => item.name));
    const pendingFiles = files.map((file) => {
      const name = dedupeName(file.name || "attachment", taken);
      taken.add(name);
      return {
        id: `upload-${requestId()}`,
        name,
        bytes: file.size,
        pending: true,
      };
    });
    setDrafts((current) => ({
      ...current,
      [id]: {
        ...(current[id] || emptyDraft()),
        files: [...(current[id]?.files || []), ...pendingFiles],
      },
    }));
    try {
      for (const [index, file] of files.entries()) {
        const asset = await uploadAttachment(
          id,
          file,
          pendingFiles[index].name,
        );
        setDrafts((current) => ({
          ...current,
          [id]: {
            ...(current[id] || emptyDraft()),
            files: (current[id]?.files || []).map((item) =>
              item.id === pendingFiles[index].id
                ? { ...asset, name: pendingFiles[index].name }
                : item,
            ),
          },
        }));
      }
      return true;
    } catch (failure) {
      report(failure);
      return false;
    } finally {
      const pendingIds = new Set(pendingFiles.map((item) => item.id));
      setDrafts((current) => ({
        ...current,
        [id]: {
          ...(current[id] || emptyDraft()),
          files: (current[id]?.files || []).filter(
            (item) => !pendingIds.has(item.id),
          ),
        },
      }));
    }
  }
  const inspect = useCallback(
    (id: string) =>
      setInspector({
        title: id.startsWith("t-") ? "Tool input/output" : "Full content",
        raw: { id, session: selected },
      }),
    [selected],
  );
  const messageActions = useMemo(
    () => ({
      report,
      recall: recallGuidance,
      branch: branchFrom,
      inspect,
      http: showMessageHttp,
      activity: openActivity,
      statistics: showMessageStatistics,
    }),
    [
      report,
      recallGuidance,
      branchFrom,
      inspect,
      showMessageHttp,
      openActivity,
      showMessageStatistics,
    ],
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
  function showContext() {
    const prepare =
      session?.generation && !running && online ? session : undefined;
    setModal({
      type: "raw",
      context: true,
      session: selected,
      exchanges: prepare ? [] : snapshots.get()[selected]?.state?.http || [],
      prepare,
    });
  }

  const open = useCallback((value: AppModal) => {
    setDrawer(false);
    setModal(value);
  }, []);
  const conversationMenu = useCallback(
    (item: Session) => (
      <ConversationMenu
        item={item}
        online={online}
        refresh={refresh}
        choose={choose}
        loadSnapshot={load}
        report={report}
        open={open}
      />
    ),
    [online, refresh, choose, load, report, open],
  );
  // The sidebar lists conversations, not scheduled runs or their tasks.
  const listed = useMemo(() => {
    const runs = new Set(
      catalogue.scheduled?.runs?.map((run) => run.session_id),
    );
    return catalogue.sessions.filter(
      (entry) => !entry.task_id && !runs.has(entry.id),
    );
  }, [catalogue.sessions, catalogue.scheduled]);
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
    />
  );

  return (
    <TimePrefsContext.Provider value={timePrefs}>
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
              {(() => {
                const blocked = Object.values(drafts).some(
                  (item) => item.text || item.files.length,
                )
                  ? "Send or copy unsent drafts first."
                  : waiting
                    ? "Wait for the running turn to finish."
                    : "";
                return (
                  <Button
                    variant="primary"
                    disabled={!!blocked}
                    title={
                      blocked || "Reloads this view with the latest fixes."
                    }
                    onClick={() =>
                      import("../shared/pwa.ts").then(({ applyUpdate }) =>
                        applyUpdate(update),
                      )
                    }
                  >
                    Refresh now
                  </Button>
                );
              })()}
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
          <div class="shell">
            {!compact ? (
              <aside class="sidebar" aria-label="Projects and sessions">
                {sidebar}
              </aside>
            ) : (
              drawer && (
                <Modal
                  title="Sessions"
                  className="sidebar drawer"
                  close={() => setDrawer(false)}
                >
                  {sidebar}
                </Modal>
              )
            )}
            <main class="conversation">
              <header class="conversation-head">
                {compact && (
                  <IconButton
                    label="Open sessions"
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
                      editInstructions={() => open({ type: "instructions" })}
                    />
                  )}
                </div>
                {browserAvailable && (
                  <IconButton
                    label={
                      browsing ? "Open browser, agent working" : "Open browser"
                    }
                    class="browser-toggle"
                    disabled={booting}
                    onClick={() => setModal({ type: "browser" })}
                  >
                    <Globe2 />
                    {browsing && <StatusLed state="running" />}
                  </IconButton>
                )}
                {compact && (
                  <div class="conversation-head-actions">
                    <IconButton
                      label="Settings"
                      onClick={() => open({ type: "settings" })}
                    >
                      <Settings />
                    </IconButton>
                  </div>
                )}
                {page === "chat" &&
                  (session ? (
                    conversationMenu(session)
                  ) : opening ? (
                    <MenuPlaceholder />
                  ) : null)}
              </header>
              {page !== "chat" && (
                <Deferred
                  key={page === "library" ? `library:${libraryKind}` : page}
                  load={page === "library" ? libraryModule : scheduledModule}
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
                actions={messageActions}
                setModal={setModal}
                setInspector={setInspector}
                showContext={showContext}
              />
            </main>
          </div>
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
          showContext={showContext}
          folder={folder}
          setFolder={setFolder}
          create={create}
          busy={busy}
          preferences={{
            theme,
            setTheme,
            timePrefs,
            setTimePrefs,
            zoom,
            setZoom,
            install,
            setInstall,
            update,
            drafts,
            uploading,
            notificationMode,
            setNotificationMode,
            notifications,
            logout,
          }}
        />
      </ImageViewer.Provider>
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
