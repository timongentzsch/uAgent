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
import { readStored, writeStored } from "./store.ts";
import { api, command, requestId } from "./api.ts";
import { Mark, Modal, Deferred, Skeleton } from "./ui.tsx";
import {
  ComposerSkeleton,
  ConversationActionSkeleton,
  HistorySkeleton,
  ManagementSkeleton,
  PairingSkeleton,
  PromptSkeleton,
  RawSkeleton,
  SettingsSkeleton,
  SidebarSkeleton,
  StatsSkeleton,
} from "./loading.tsx";

import { useHost } from "./use-host.ts";
import { parseSlash } from "./slash.ts";
import { useTranscriptScroll } from "./use-transcript-scroll.ts";
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
const chat = () => import("./chat.tsx");
const rawDialog = () => import("./raw.tsx");
const conversationActions = () => import("./conversation-actions.tsx");
const statisticsDialog = () => import("./statistics.tsx");
const promptDialog = () => import("./prompt.tsx");
const settingsDialog = () => import("./settings.tsx");

const emptyDraft = (): Draft => ({ text: "", files: [] });
const noBlocks: Block[] = [];
function App() {
  const [page, setPage] = useState<"chat" | "library" | "scheduled">("chat");
  const [drawer, setDrawer] = useState(false);
  const [compact, setCompact] = useState(
    () => matchMedia("(max-width: 900px)").matches,
  );
  const [modal, setModal] = useState<AppModal | null>(null);
  const [notice, setNotice] = useState("");
  const onResult = useCallback((value: JSONValue, inspect: boolean) => {
    if (inspect) setModal({ type: "raw", value });
    else setNotice(typeof value === "string" ? value : JSON.stringify(value));
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
    correlate,
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
  const transcriptContent = useRef<HTMLDivElement>(null);
  const { jumpToLatest, preservePrepend } = useTranscriptScroll(
    transcript,
    transcriptContent,
    setFollowing,
  );
  const [activityTarget, setActivityTarget] = useState<Block | null>(null);
  const snapshot = snapshots[selected];
  const session =
    snapshot?.metadata ||
    catalogue.sessions.find((item) => item.id === selected);
  const draft = drafts[selected] || emptyDraft();
  const pending = snapshot?.pending;
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
      }),
    [selected],
  );
  const view = snapshot?.state?.view;
  const streamed = snapshot?.streamed || noBlocks;
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
  useLayoutEffect(() => {
    jumpToLatest();
  }, [selected, page, jumpToLatest]);
  useEffect(() => {
    let viewport: (() => void) | undefined;
    let compact: (() => void) | undefined;
    import("./layout.ts").then(({ trackViewport, observeCompact }) => {
      viewport = trackViewport();
      compact = observeCompact((value) => {
        setCompact(value);
        setDrawer(false);
      });
    });
    return () => {
      viewport?.();
      compact?.();
    };
  }, []);
  useEffect(() => {
    let stop: (() => void) | undefined;
    import("./layout.ts").then(({ applyTheme }) => (stop = applyTheme(theme)));
    return () => stop?.();
  }, [theme]);
  useEffect(() => {
    let stop: (() => void) | undefined;
    import("./pwa.ts").then(
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
    setNotice("");
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
  // Recall returns queued guidance to the composer while it is still
  // queued. Delivered guidance belongs to the turn; dropping the row is
  // then the only correct move.
  const recallGuidance = useCallback(
    async (block: Block) => {
      const target = block.request_id;
      if (!target || block.status !== "Guidance queued" || !online) return;
      const text = block.text || "";
      const id = selected;
      try {
        await act("recall", { target_id: target });
      } catch (error) {
        const issue = failure(error);
        if (!/already delivered/i.test(issue.message)) {
          report(error);
          return;
        }
      }
      setOutgoing((items) =>
        items.filter((item) => item.request_id !== target),
      );
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
    },
    [online, selected, act, report],
  );
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
  const inspect = useCallback(
    (id: string) => {
      setModal({ type: "raw", id, session: selected });
    },
    [selected],
  );
  async function older() {
    if (!view?.more || !transcript.current) return;
    const before = view.before;
    const id = selected;
    await preservePrepend(async () => {
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
      return applied;
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
    const prepare =
      session?.generation && !running && online ? session : undefined;
    setModal({
      type: "raw",
      context: true,
      session: selected,
      exchanges: prepare ? [] : snapshot?.state?.http || [],
      prepare,
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
      {(error || notice) && (
        <div role={error ? "alert" : "status"} class="error-banner">
          <span>{error || notice}</span>
          <button
            onClick={() => {
              setError("");
              setNotice("");
            }}
          >
            Dismiss
          </button>
        </div>
      )}
      {authenticated === false ? (
        <Deferred
          load={pairing}
          paired={refresh}
          report={report}
          fallback={<PairingSkeleton />}
        />
      ) : authenticated === null ? (
        <main class="shell loading-shell">
          {!compact && <SidebarSkeleton />}
          <div class="conversation">
            <header class="conversation-head">
              <Skeleton rows={1} className="title-skeleton" />
            </header>
            <div class="transcript">
              <HistorySkeleton />
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
                  <span aria-hidden="true">☰</span>
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
              {compact && (
                <button
                  class="quiet icon-button"
                  onClick={() => open({ type: "settings" })}
                  aria-label="Settings"
                >
                  <span aria-hidden="true">⚙</span>
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
                fallback={<ManagementSkeleton />}
              />
            ) : session ? (
              <>
                <Deferred
                  load={chat}
                  fallback={
                    <div className="transcript">
                      <div className="transcript-content">
                        <HistorySkeleton />
                      </div>
                    </div>
                  }
                  scroller={transcript}
                  content={transcriptContent}
                  selected={selected}
                  snapshot={snapshot}
                  loadError={loadErrors[selected]}
                  blocks={blocks}
                  session={session}
                  online={online}
                  loadSnapshot={load}
                  older={older}
                  report={report}
                  recall={recallGuidance}
                  inspect={inspect}
                  http={showMessageHttp}
                  activity={setActivityTarget}
                  statistics={showMessageStatistics}
                />
                <Deferred
                  load={composer}
                  fallback={<ComposerSkeleton />}
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
                  // Optimistic: pin to the end synchronously (<1 frame),
                  // refresh the snapshot in the background. jumpToLatest
                  // is idempotent and load() dedupes in flight, so rapid
                  // presses stay a single pin + a single fetch.
                  jump={() => {
                    jumpToLatest();
                    load(selected).catch(report);
                  }}
                  activityTarget={activityTarget}
                  clearActivity={() => setActivityTarget(null)}
                  showStatistics={() =>
                    setModal({ type: "statistics", session_id: selected })
                  }
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
                : "block_id" in modal && modal.block_id
                  ? "Message statistics"
                  : "Conversation statistics"
          }
          className={modal.type === "statistics" ? "statistics-view" : ""}
          close={() => setModal(null)}
        >
          {modal.type === "statistics" ? (
            <Deferred
              load={statisticsDialog}
              fallback={<StatsSkeleton />}
              modal={modal}
              loadSnapshot={load}
            />
          ) : (
            <Deferred
              load={conversationActions}
              fallback={
                <ConversationActionSkeleton kind={modal.type} />
              }
              key={`${modal.type}-${modal.session.id}`}
              modal={modal}
              close={() => setModal(null)}
              online={online}
              changed={async (kind: string, id: string) => {
                if (kind === "delete") forget(id);
                await refresh();
              }}
            />
          )}
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
          className="prompt-view"
          close={() => setModal(null)}
        >
          <Deferred
            load={promptDialog}
            fallback={<PromptSkeleton />}
            session={session}
            projects={projects}
            online={online}
            version={managementVersion}
            scope={modal.scope}
            edit={modal.edit}
            lastSent={snapshot?.state?.system_prompt}
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
