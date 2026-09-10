import type {
  AppModal,
  JSONValue,
  Draft,
  Sizes,
  InstallPrompt,
  Block,
  Session,
  Snapshot,
  Asset,
  Act,
  CommandKind,
  CommandFields,
} from "./types.ts";
import { failure } from "./types.ts";
import type { JSX } from "preact";
import { render } from "preact";
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import {
  api,
  command,
  liveBlocks,
  requestId,
  readStored,
  writeStored,
} from "./store.ts";
import { PanelLeft, Settings } from "lucide-preact";
import { Mark, Modal, Deferred, Skeleton, LoadError } from "./ui.tsx";
import {
  ManagementPageSkeleton,
  ComposerSkeleton,
  SidebarSkeleton,
  RawSkeleton,
  SettingsSkeleton,
  StatsSkeleton,
} from "./loading.tsx";

import { useHost } from "./use-host.ts";
import { parseSlash } from "./slash.ts";
import { selectedFromURL } from "./navigation.ts";
import { observeResize, trackViewport } from "./layout.ts";
import "./style.css";
const sidebarModule = () => import("./sidebar.tsx");
const menuModule = () =>
  import("./sidebar.tsx").then((module) => ({
    default: module.ConversationMenu,
  }));
const libraryModule = () => import("./library.tsx");
const scheduledModule = () => import("./scheduled.tsx");
const pairing = () => import("./pairing.tsx");
const composer = () => import("./composer.tsx");
const messages = () => import("./message.tsx");
const rawDialog = () => import("./raw.tsx");
const statisticsDialog = () => import("./statistics.tsx");
const promptDialog = () => import("./prompt.tsx");
const settingsDialog = () => import("./settings.tsx");

const emptyDraft = (): Draft => ({ text: "", files: [] });
function App() {
  const [page, setPage] = useState<"chat" | "library" | "scheduled">("chat");
  const [drawer, setDrawer] = useState(false);
  const [compact, setCompact] = useState(
    () => matchMedia("(max-width: 900px)").matches,
  );
  const [modal, setModal] = useState<AppModal | null>(null);
  const onResult = useCallback((value: JSONValue) => {
    if (
      value &&
      typeof value === "object" &&
      !Array.isArray(value) &&
      value.editor === true
    ) {
      setModal({
        type: "prompt",
        scope: String(value.scope || "conversation"),
        edit: true,
      });
    } else setModal({ type: "raw", value });
  }, []);
  const {
    managementVersion,
    authenticated,
    online,
    connecting,
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
    reset,
  } = useHost(onResult, page === "chat");
  const [folder, setFolder] = useState("");
  const [busy, setBusy] = useState(false);
  const [uploading, setUploading] = useState(false);
  const [sizes, setSizes] = useState(() =>
    readStored<Sizes>(localStorage, "uagent-sizes", {
      display: 100,
      text: 100,
    }),
  );
  const [install, setInstall] = useState<InstallPrompt | null>(null);
  const [update, setUpdate] = useState<ServiceWorker | null>(null);
  const [notificationMode, setNotificationMode] = useState(false);
  const [theme, setTheme] = useState(
    () => localStorage.getItem("uagent-theme") || "system",
  );
  const transcript = useRef<HTMLDivElement>(null);
  const scrollPositions = useRef(new Map<string, number>());
  const restoredScroll = useRef<{
    element: HTMLElement;
    top: number;
    height: number;
    client: number;
  }>();
  const [activityTarget, setActivityTarget] = useState<Block | null>(null);
  const snapshot = snapshots[selected];
  const session =
    snapshot?.metadata ||
    catalogue.sessions.find((item) => item.id === selected);
  const draft = drafts[selected] || emptyDraft();
  const pending = snapshot?.pending;
  const running = !!session?.turn_active;
  const view = snapshot?.state?.view;
  const streamed = useMemo(
    () => snapshot?.streamed || liveBlocks(snapshot?.live || []),
    [snapshot?.live, snapshot?.streamed],
  );
  const blocks = useMemo(
    () => [
      ...(view?.blocks || []),
      ...outgoing.filter(
        (item) =>
          item.session_id === selected &&
          !view?.blocks?.some((block) => block.request_id === item.request_id),
      ),
      ...streamed,
    ],
    [view?.blocks, streamed, outgoing, selected],
  );

  function setDraft(value: Draft, id = selected) {
    setDrafts((current) => ({ ...current, [id]: value }));
  }
  useEffect(() => {
    writeStored(localStorage, "uagent-sizes", sizes);
    for (const [key, value] of Object.entries(sizes))
      document.documentElement.style.setProperty(
        `--${key}-scale`,
        String(
          Math.min(
            key === "text" ? 300 : 200,
            Math.max(50, Number(value) || 100),
          ) / 100,
        ),
      );
  }, [sizes]);
  const restoreScroll = useCallback(() => {
    const element = transcript.current;
    if (!element) return;
    element.scrollTop = following
      ? element.scrollHeight
      : scrollPositions.current.get(selected) || 0;
    // A clamped restoration is not a user scroll or a request to follow output.
    restoredScroll.current = {
      element,
      top: element.scrollTop,
      height: element.scrollHeight,
      client: element.clientHeight,
    };
  }, [selected, following]);
  useLayoutEffect(restoreScroll, [restoreScroll, blocks]);
  useEffect(() => {
    const element = transcript.current;
    if (element) return observeResize(restoreScroll, element);
  }, [restoreScroll, session?.id, page]);
  useEffect(() => {
    const media = matchMedia("(max-width: 900px)");
    const changed = () => {
      setCompact(media.matches);
      setDrawer(false);
    };
    media.addEventListener("change", changed);
    return () => media.removeEventListener("change", changed);
  }, []);
  useEffect(trackViewport, []);
  useEffect(() => {
    const media = matchMedia("(prefers-color-scheme: dark)");
    const apply = () => {
      const resolved =
        theme === "system" ? (media.matches ? "dark" : "light") : theme;
      document.documentElement.dataset.theme = resolved;
      document.querySelector<HTMLMetaElement>(
        'meta[name="theme-color"]',
      )!.content = resolved === "dark" ? "#000000" : "#ffffff";
    };
    apply();
    localStorage.setItem("uagent-theme", theme);
    media.addEventListener("change", apply);
    return () => media.removeEventListener("change", apply);
  }, [theme]);
  useEffect(() => {
    const prompt = (event: Event) => {
      event.preventDefault();
      setInstall(event as InstallPrompt);
    };
    addEventListener("beforeinstallprompt", prompt);
    if ("serviceWorker" in navigator && isSecureContext) {
      navigator.serviceWorker
        .register("/sw.js", { updateViaCache: "none" })
        .then((registration) => {
          if (registration.waiting) setUpdate(registration.waiting);
          registration.addEventListener("updatefound", () => {
            const worker = registration.installing;
            worker?.addEventListener("statechange", () => {
              if (
                worker.state === "installed" &&
                navigator.serviceWorker.controller
              )
                setUpdate(worker);
            });
          });
        })
        .catch(report);
    }
    return () => removeEventListener("beforeinstallprompt", prompt);
  }, [report]);

  const act: Act = async <K extends CommandKind>(
    kind: K,
    fields: CommandFields = {},
  ) => {
    if (!online)
      throw new Error("Reconnect and refresh before sending commands.");
    const result = await command(kind, session, fields);
    return result;
  };
  useEffect(() => {
    const openSession = (event: MessageEvent) => {
      if (
        event.data?.type === "OPEN_SESSION" &&
        /^[a-f0-9]{16,64}$/.test(event.data.id || "")
      )
        choose(event.data.id);
    };
    const back = () => {
      setPage("chat");
      setDrawer(false);
      setModal(null);
    };
    navigator.serviceWorker?.addEventListener("message", openSession);
    addEventListener("popstate", back);
    return () => {
      navigator.serviceWorker?.removeEventListener("message", openSession);
      removeEventListener("popstate", back);
    };
  }, []);
  async function choose(id: string) {
    setPage("chat");
    setSelected(id);
    setDrawer(false);
  }
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
  async function startConversation(cwd: string) {
    const created = await command("create", null, { cwd });
    if (created.pending) return;
    setCatalogue((prior) => ({
      ...prior,
      sessions: [created.session, ...prior.sessions],
    }));
    await choose(created.session.id);
    await command("activate", created.session);
    await load(created.session.id);
  }
  async function localCommand(text: string) {
    const { name, argument } = parseSlash(catalogue.commands || [], text);
    if (name === "/context") showContext();
    else if (name === "/sessions") setDrawer(true);
    else if (name === "/reset") await startConversation(session!.cwd!);
    else if (name === "/quit") {
      await act("close");
      await load(selected);
    } else if (name === "/fork") {
      const result = await command("fork", session, { title: argument });
      if (!result.pending) {
        await refresh();
        await choose(result.result.id);
        await command("activate", { id: result.result.id, generation: "" });
        await load(result.result.id);
      }
    } else if (
      name === "/prompt" &&
      (!argument ||
        /^(show|edit)(?: --scope (global|project|conversation))?$/.test(
          argument,
        ))
    ) {
      setModal({
        type: "prompt",
        scope: argument.match(/--scope (\w+)/)?.[1],
        edit: argument.startsWith("edit"),
      });
    } else if (name === "/http") {
      const exchanges = snapshot?.state?.http || [];
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
    } else if (name === "/trace" && argument) inspect(argument);
    else return false;
    return true;
  }
  async function submit(event: Event) {
    event.preventDefault();
    if (
      !online ||
      pending ||
      (!draft.text.trim() && !draft.files.length) ||
      uploading ||
      busy
    )
      return;
    setBusy(true);
    const id = selected,
      sent = draft,
      request_id = requestId();
    if (sent.text.startsWith("/")) {
      try {
        if (running)
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
    if (kind === "steer" && sent.files.length) {
      report(new Error("Attachments must be sent with a new turn."));
      setBusy(false);
      return;
    }
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
        ...(kind === "submit"
          ? { attachment_ids: sent.files.map((item) => item.id) }
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
  async function upload(files: File[]) {
    if (!session || !online || uploading || !files.length) return;
    const id = selected;
    if (
      files.length + draft.files.length > 8 ||
      files.some((file) => file.size > 8 * 1024 * 1024)
    ) {
      report(new Error("Attach up to 8 files, at most 8 MiB each."));
      return;
    }
    const pendingFiles = files.map((file) => ({
      id: `upload-${requestId()}`,
      name: file.name || "attachment",
      bytes: file.size,
      pending: true,
    }));
    setUploading(true);
    setDrafts((current) => ({
      ...current,
      [id]: {
        ...(current[id] || emptyDraft()),
        files: [...(current[id]?.files || []), ...pendingFiles],
      },
    }));
    try {
      for (const [index, file] of files.entries()) {
        const asset = await api<Asset>(
          `/api/sessions/${id}/attachments?name=${encodeURIComponent(pendingFiles[index].name)}`,
          undefined,
          {
            method: "POST",
            headers: {
              "Content-Type": file.type || "application/octet-stream",
            },
            body: file,
          },
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
    } catch (failure) {
      report(failure);
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
      setUploading(false);
    }
  }
  function inspect(id: string) {
    setModal({ type: "raw", id, session: selected });
  }
  async function older() {
    setFollowing(false);
    if (!view?.more || !transcript.current) return;
    const before = view.before;
    const id = selected;
    const element = transcript.current,
      height = element.scrollHeight;
    const value = await api<Snapshot>(`/api/sessions/${id}?before=${before}`);

    let applied = false;
    updateView(id, (current) => {
      if (
        current.epoch !== value.epoch ||
        current.metadata.generation !== value.metadata.generation ||
        current.state?.view?.before !== before
      )
        return current;
      applied = true;
      return {
        ...current,
        state: {
          ...current.state,
          view: {
            ...value.state?.view,
            blocks: [
              ...(value.state?.view?.blocks || []),
              ...(current.state?.view?.blocks || []),
            ].slice(0, 256),
          },
        },
      };
    });
    if (!applied) return;
    requestAnimationFrame(() => {
      if (element.isConnected && element.dataset.session === id) {
        element.scrollTop += element.scrollHeight - height;
        scrollPositions.current.set(id, element.scrollTop);
      }
    });
  }
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
    const exchanges = snapshot?.state?.http || [];
    setModal({
      type: "raw",
      context: true,
      session: selected,
      exchanges,
      prepare:
        !exchanges.length && session?.generation && !running
          ? session
          : undefined,
    });
  }
  const open = (value: AppModal) => {
    setDrawer(false);
    setModal(value);
  };
  const conversationMenu = (item: Session) => (
    <Deferred
      load={menuModule}
      fallback={null}
      item={item}
      online={online}
      refresh={refresh}
      choose={choose}
      loadSnapshot={load}
      report={report}
      open={open}
    />
  );
  const ios =
    /iPhone|iPad/.test(navigator.userAgent) ||
    (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);
  const installed =
    matchMedia("(display-mode: standalone)").matches ||
    ("standalone" in navigator && navigator.standalone === true);

  const projects = [
    ...new Set(
      catalogue.sessions
        .map((entry) => entry.cwd)
        .filter((path): path is string => !!path),
    ),
  ];
  const sidebar = (
    <Deferred
      load={sidebarModule}
      fallback={<SidebarSkeleton />}
      page={page}
      navigate={(value) => {
        setPage(value);
        setDrawer(false);
      }}
      scheduledUnread={
        !!catalogue.scheduled?.runs?.some((run) => unread.has(run.session_id))
      }
      sessions={catalogue.sessions.filter(
        (entry) =>
          !entry.task_id &&
          !catalogue.scheduled?.runs?.some(
            (run) => run.session_id === entry.id,
          ),
      )}
      selected={page === "chat" ? selected : ""}
      unread={unread}
      online={online}
      connecting={connecting}
      choose={choose}
      menu={conversationMenu}
      refresh={refresh}
      settings={() => open({ type: "settings" })}
      create={() => {
        setFolder(session?.cwd || "");
        open({ type: "new" });
      }}
    />
  );

  return (
    <>
      {error && (
        <div role="alert" class="error-banner">
          <span>{error}</span>
          <button onClick={() => setError("")}>Dismiss</button>
        </div>
      )}
      {authenticated === false ? (
        <Deferred
          load={pairing}
          paired={refresh}
          report={report}
          fallback={
            <main class="pairing">
              <Mark className="wordmark" />
              <Skeleton rows={5} label="Loading connection form…" />
            </main>
          }
        />
      ) : authenticated === null ? (
        <main class="shell loading-shell">
          {!compact && <SidebarSkeleton />}
          <div class="conversation">
            <header class="conversation-head">
              <Skeleton rows={1} />
            </header>
            <div class="transcript">
              <Skeleton className="history-skeleton" rows={8} />
            </div>
            <ComposerSkeleton />
          </div>
        </main>
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
                <button
                  class="quiet icon-button"
                  onClick={() => setDrawer(true)}
                  aria-label="Open sessions"
                >
                  <PanelLeft />
                </button>
              )}
              <div>
                <h1 title={session?.cwd}>
                  {page === "library"
                    ? "Library"
                    : page === "scheduled"
                      ? "Scheduled"
                      : session?.title || "Your workspace"}
                </h1>
              </div>
              {session && <span class="status sr-only">{session.status}</span>}
              {compact && (
                <button
                  class="quiet icon-button"
                  onClick={() => open({ type: "settings" })}
                  aria-label="Settings"
                >
                  <Settings />
                </button>
              )}
              {page === "chat" && session && conversationMenu(session)}
            </header>
            {page !== "chat" ? (
              <Deferred
                key={page}
                load={page === "library" ? libraryModule : scheduledModule}
                projects={projects}
                cwd={session?.cwd || projects[0] || ""}
                online={online}
                version={managementVersion}
                scheduled={catalogue.scheduled}
                unread={unread}
                choose={choose}
                refresh={refresh}
                fallback={<ManagementPageSkeleton compact={compact} />}
              />
            ) : session ? (
              <>
                <div
                  class="transcript"
                  key={selected}
                  data-session={selected}
                  aria-busy={(!snapshot && !loadErrors[selected]) || undefined}
                  ref={transcript}
                  onScroll={(event) => {
                    const element = event.currentTarget;
                    if (
                      !element.isConnected ||
                      element.dataset.session !== selectedFromURL()
                    )
                      return;
                    const restored = restoredScroll.current;
                    if (
                      restored?.element === element &&
                      (restored.height !== element.scrollHeight ||
                        restored.client !== element.clientHeight)
                    ) {
                      // Layout can clamp scroll before ResizeObserver restores it.
                      restoreScroll();
                      return;
                    }
                    if (
                      restored?.element === element &&
                      restored.top === element.scrollTop
                    )
                      return;
                    scrollPositions.current.set(selected, element.scrollTop);
                    if (scrollPositions.current.size > 64)
                      scrollPositions.current.delete(
                        scrollPositions.current.keys().next().value!,
                      );
                    setFollowing(
                      element.scrollHeight -
                        element.scrollTop -
                        element.clientHeight <
                        80,
                    );
                  }}
                >
                  {view?.more && (
                    <button
                      class="history-button"
                      onClick={() => older().catch(report)}
                    >
                      Load older retained messages
                    </button>
                  )}
                  {(view?.dropped_segments || 0) > 0 && (
                    <p class="retention">
                      {view?.dropped_segments} older segments are outside
                      retention.
                    </p>
                  )}
                  {snapshot?.live_truncated && (
                    <p class="retention">
                      The live preview exceeded its buffer. Retained history
                      refreshes when this turn saves.
                    </p>
                  )}
                  {!snapshot &&
                    (loadErrors[selected] ? (
                      <LoadError
                        error={loadErrors[selected]}
                        retry={() => load(selected).catch(() => {})}
                      />
                    ) : (
                      <Skeleton
                        className="history-skeleton"
                        rows={8}
                        label="Loading conversation…"
                      />
                    ))}
                  {snapshot && blocks.length === 0 && (
                    <div class="empty">
                      <Mark className="cursor-mark" />
                      <h2>What are we working on?</h2>
                      <p>
                        Describe a task, attach a file, or use an existing slash
                        command.
                      </p>
                    </div>
                  )}
                  {snapshot && (
                    <Deferred
                      load={messages}
                      blocks={blocks}
                      restoreScroll={restoreScroll}
                      session={session}
                      report={report}
                      inspect={inspect}
                      http={(exchanges) =>
                        setModal({ type: "raw", session: selected, exchanges })
                      }
                      image={(url) => setModal({ type: "image", url })}
                      activity={setActivityTarget}
                      statistics={(block) =>
                        setModal({ type: "statistics", block })
                      }
                      fallback={
                        <Skeleton
                          className="history-skeleton"
                          rows={6}
                          label="Loading messages…"
                        />
                      }
                    />
                  )}
                  {session?.error && <p class="failure">{session.error}</p>}
                  {snapshot?.state?.error && (
                    <p class="failure">{snapshot.state.error}</p>
                  )}
                </div>
                <Deferred
                  load={composer}
                  fallback={<ComposerSkeleton />}
                  key={selected}
                  session={session}
                  commands={catalogue.commands || []}
                  snapshot={snapshot}
                  online={online}
                  draft={draft}
                  setDraft={setDraft}
                  upload={upload}
                  uploading={uploading}
                  busy={busy}
                  submit={submit}
                  act={act}
                  report={report}
                  following={following}
                  jump={() =>
                    load(selected)
                      .then(() => setFollowing(true))
                      .catch(report)
                  }
                  activityTarget={activityTarget}
                  clearActivity={() => setActivityTarget(null)}
                  showContext={showContext}
                  sizes={sizes}
                />
              </>
            ) : (
              <div class="empty">
                <Mark className="cursor-mark" />
                <h1>Your projects. One workspace.</h1>
                <p>
                  Open a saved session or start in any directory on your host.
                </p>
                <button
                  class="primary"
                  onClick={() => setModal({ type: "new" })}
                  disabled={!online}
                >
                  New conversation
                </button>
              </div>
            )}
          </main>
        </div>
      )}
      {(modal?.type === "statistics" ||
        modal?.type === "rename" ||
        modal?.type === "delete") && (
        <Modal
          title={
            modal.type === "rename"
              ? "Rename conversation"
              : modal.type === "delete"
                ? "Delete conversation"
                : modal.block
                  ? "Message statistics"
                  : "Conversation statistics"
          }
          className={modal.type === "statistics" ? "statistics-view" : ""}
          close={() => setModal(null)}
        >
          <Deferred
            load={statisticsDialog}
            fallback={
              modal.type === "statistics" ? (
                <StatsSkeleton />
              ) : (
                <Skeleton className="form-skeleton" rows={1} />
              )
            }
            key={`${modal.type}-${modal.session?.id || modal.block?.id || "session"}`}
            modal={modal}
            loadSnapshot={load}
            close={() => setModal(null)}
            online={online}
            changed={async (kind: string, id: string) => {
              if (kind === "delete") forget(id);
              await refresh();
            }}
          />
        </Modal>
      )}
      {modal?.type === "new" && (
        <Modal title="New conversation" close={() => setModal(null)}>
          <form onSubmit={create}>
            <label>
              Directory on the host
              <input
                value={folder}
                onInput={(event) => setFolder(event.currentTarget.value)}
                placeholder="/path/to/project"
                required
                autoComplete="off"
              />
            </label>
            <p class="muted">
              Any accessible directory works, including a folder outside Git.
              Multiple conversations can work in the same folder.
            </p>
            <button class="primary" disabled={busy || !online}>
              Start conversation
            </button>
          </form>
        </Modal>
      )}
      {modal?.type === "image" && (
        <Modal title="Image" className="raw-view" close={() => setModal(null)}>
          <img
            class="full-image"
            src={modal.url}
            alt="Full-size attached image"
          />
        </Modal>
      )}
      {modal?.type === "raw" && (
        <Modal
          title={
            modal.context
              ? "Raw context"
              : modal.exchanges !== undefined
                ? "HTTP request/response"
                : modal.id?.startsWith("t-")
                  ? "Tool input/output"
                  : "Full content"
          }
          className="raw-view"
          close={() => setModal(null)}
        >
          <Deferred
            load={rawDialog}
            prompt={() => setModal({ type: "prompt" })}
            fallback={
              <RawSkeleton
                http={modal.context || modal.exchanges !== undefined}
              />
            }
            id={modal.id}
            session={modal.session}
            value={modal.value}
            exchanges={modal.exchanges}
            latest={
              modal.session ? snapshots[modal.session]?.state?.http : undefined
            }
            context={modal.context}
            prepare={modal.prepare}
            part={modal.part}
          />
        </Modal>
      )}
      {modal?.type === "prompt" && (
        <Modal
          title="System prompt"
          className="raw-view"
          close={() => setModal(null)}
        >
          <Deferred
            load={promptDialog}
            fallback={<Skeleton rows={12} label="Loading system prompt…" />}
            session={session}
            projects={projects}
            online={online}
            version={managementVersion}
            scope={modal.scope}
            edit={modal.edit}
          />
        </Modal>
      )}
      {modal?.type === "settings" && (
        <Modal
          title="Settings"
          className="settings-view"
          close={() => setModal(null)}
        >
          <Deferred
            load={settingsDialog}
            fallback={<SettingsSkeleton />}
            theme={theme}
            setTheme={setTheme}
            sizes={sizes}
            setSizes={setSizes}
            installed={installed}
            install={install}
            setInstall={setInstall}
            ios={ios}
            update={update}
            drafts={drafts}
            uploading={uploading}
            snapshots={snapshots}
            catalogue={catalogue}
            online={online}
            notificationMode={notificationMode}
            setNotificationMode={setNotificationMode}
            notifications={notifications}
            refresh={refresh}
            selected={selected}
            session={session}
            logout={logout}
            prompt={() => setModal({ type: "prompt" })}
          />
        </Modal>
      )}
    </>
  );
}
render(<App />, document.getElementById("app")!);
