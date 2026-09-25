import "./activity.css";
import MessageInput from "../composer/message-input.tsx";
import ModelControl from "../composer/model-control.tsx";
import HistoryStart from "./history-start.tsx";
import { ContextSummary, SessionSummary } from "./session-summary.tsx";
import { StatisticsLoading } from "../../shared/statistics-layout.tsx";
import { count } from "../../shared/quantities.ts";
import { duration } from "../../shared/duration.ts";
import type {
  Activity,
  ActivityDetail,
  Block,
  Collaborator,
  JSONValue,
  RawOptions,
  Report,
  SessionRef,
  State,
} from "../../shared/types.ts";
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import { ArrowDown, ArrowDownToLine, ArrowLeft, Square } from "lucide-preact";
import { command, readPages, manage } from "../../state/api.ts";
import { useTranscriptHistory } from "../../state/use-transcript-history.ts";
import { prependHistoryPage } from "../../state/history-page.ts";
import {
  cleanText,
  Deferred,
  Field,
  IconButton,
  LoadError,
  Modal,
  Spinner,
} from "../../shared/ui.tsx";
import {
  active,
  ActivityStatus,
  withCollaborators,
} from "./activity-status.tsx";
import Markdown from "../../shared/markdown-view.tsx";
import { MessageRows, prepareHistoryBlocks } from "./message.tsx";

// What the inspector is opened on: a transcript row, a live activity, or a
// tool's retained input/output.
export type InspectorTarget =
  { block: Block } | { item: Activity } | { raw: RawOptions; title: string };

// A page shown over the current detail inside the same sheet (Back returns),
// never a second dialog.
type Page = {
  title: string;
  raw?: RawOptions;
  stats?: { blockId?: string };
};

const statisticsDialog = () =>
  import("../settings/statistics.tsx").then((module) => ({
    default: module.StatisticsContent,
  }));
const rawDialog = () => import("../settings/raw.tsx");

const childStateOf = (detail: ActivityDetail): State => ({
  view: detail.conversation,
  context_tokens: detail.context_tokens,
  context_window: detail.context_window,
  usage: detail.usage,
  statistics: detail.statistics,
  turns: detail.turns,
  route: detail.route || detail.model,
});

// Pure fetch half of an activity inspect: memory content or the full
// agent/activity detail, with older-page appends merged. State (version
// guards, loading, errors) stays with the caller so panel and nested
// viewers share the loader without sharing UI state.
async function fetchActivityDetail(
  session: SessionRef,
  cwd: string,
  item: ActivityDetail,
  before?: number,
  prior?: ActivityDetail | null,
  signal?: AbortSignal,
): Promise<ActivityDetail> {
  if (item.memory) {
    const result = await manage(
      "memory",
      {
        action: "get",
        cwd,
        key: item.memory.key,
      },
      signal,
    );
    if (!result.item || result.item.error)
      throw new Error(result.item?.error || "Memory is no longer available.");
    return { ...item, output: result.item.content || "" };
  }
  const response = await command(
    "activity",
    session,
    {
      operation: "inspect",
      activity_id: item.id || item.activity_id || 0,
      agent_id: item.agent_id || "",
      ...(before ? { before } : {}),
    },
    { signal },
  );
  if (response.pending)
    throw new Error("Activity is still loading. Try again shortly.");
  await prepareHistoryBlocks(response.result.conversation?.blocks || []);
  return {
    ...item,
    ...response.result,
    olderWindow: !!before,
    ...(before && prior?.conversation && response.result.conversation
      ? {
          conversation: prependHistoryPage(
            response.result.conversation,
            prior.conversation,
          ),
        }
      : {}),
  };
}

// One inspector for everything the agent started: a command, a subagent, a
// memory or a tool's input/output. It opens as a side sheet; drilling into a
// nested agent or a tool page stays inside it with Back.
export default function Inspector({
  target,
  items,
  collaborators,
  cwd,
  running,
  session,
  online,
  report,
  close,
}: {
  target: InspectorTarget;
  items: Activity[];
  collaborators: Collaborator[];
  cwd: string;
  running?: boolean;
  session: SessionRef;
  online: boolean;
  report: Report;
  close: () => void;
}) {
  const rows = withCollaborators(items, collaborators);
  const [detail, setDetail] = useState<ActivityDetail | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const [page, setPage] = useState<Page | null>(null);
  const [busy, setBusy] = useState(false);
  const inspection = useRef(0);
  const inspectionRequest = useRef<AbortController>();
  // Progress ticks can outpace a refresh: one runs at a time and the latest
  // tick waits for it, instead of each tick aborting the one before.
  const refreshing = useRef(false);
  const queuedRefresh = useRef<ActivityDetail | null>(null);

  useEffect(
    () => () => {
      ++inspection.current;
      inspectionRequest.current?.abort();
    },
    [],
  );

  // Bound loader shared with nested viewers (which own their UI state).
  const loadDetail = useCallback(
    (
      item: ActivityDetail,
      before?: number,
      prior?: ActivityDetail | null,
      signal?: AbortSignal,
    ) => fetchActivityDetail(session, cwd, item, before, prior, signal),
    [session, cwd],
  );

  async function inspect(
    item: ActivityDetail,
    before?: number,
    refresh = false,
  ) {
    if (refresh && refreshing.current) {
      queuedRefresh.current = item;
      return;
    }
    refreshing.current = refresh;
    const version = ++inspection.current;
    inspectionRequest.current?.abort();
    const controller = new AbortController();
    inspectionRequest.current = controller;
    if (!before)
      setDetail((prior) =>
        prior?.id === item.id && prior?.agent_id === item.agent_id
          ? prior
          : item,
      );
    if (!refresh) {
      setLoading(true);
      setError(null);
    }
    try {
      const next = await loadDetail(
        item,
        before,
        before ? detail : undefined,
        controller.signal,
      );
      if (version !== inspection.current) return;
      setDetail(next);
    } catch (failure) {
      if (version === inspection.current && !refresh) setError(failure);
    } finally {
      if (version === inspection.current && !refresh) setLoading(false);
      if (refresh) {
        refreshing.current = false;
        const queued = queuedRefresh.current;
        queuedRefresh.current = null;
        if (queued && version === inspection.current)
          inspect(queued, undefined, true).catch(report);
      }
    }
  }

  // A transcript row carries the command it ran (bounded 8k) and its text, so
  // a job the supervisor no longer retains still opens with its record.
  useEffect(() => {
    setPage(null);
    if ("raw" in target) {
      ++inspection.current;
      setDetail(null);
      setPage({ title: target.title, raw: target.raw });
      return;
    }
    if ("item" in target) {
      inspect(target.item).catch(report);
      return;
    }
    const { block } = target;
    const item = rows.find((row) => row.id === block.activity_id);
    const receipt: ActivityDetail = {
      ...item,
      activity_id: block.activity_id,
      agent_id: block.agent_id,
      label: block.activity?.label || "Recorded event",
      status: block.status,
      memory: block.memory,
      command: block.command || item?.command,
      output: block.text,
    };
    if (item || block.agent_id || block.memory) inspect(receipt).catch(report);
    else {
      ++inspection.current;
      setDetail(receipt);
      setError(null);
      setLoading(false);
    }
  }, [target]);

  // The parent stream already reports collaborator and activity progress.
  // Refresh an open child only when that authoritative state changes instead
  // of running a second polling clock beside SSE.
  const rowsVersion = rows
    .map((item) =>
      [
        item.id,
        item.agent_id,
        item.status,
        item.progress,
        item.duration_ms,
      ].join(":"),
    )
    .join("|");
  useEffect(() => {
    if (!detail || loading || detail.olderWindow) return;
    const item = rows.find((item) =>
      detail.agent_id
        ? item.agent_id === detail.agent_id
        : item.id === detail.id,
    );
    inspect(item || detail, undefined, true).catch(report);
  }, [rowsVersion, detail?.id, detail?.agent_id, detail?.olderWindow, loading]);

  const current =
    (detail &&
      rows.find((item) =>
        detail.agent_id
          ? item.agent_id === detail.agent_id
          : item.id === detail.id,
      )) ||
    detail;
  const isAgent = !!detail?.agent_id && !detail.memory;
  const title =
    page?.title ||
    (detail?.memory
      ? "Memory"
      : isAgent
        ? detail?.name || "Subagent"
        : "Activity");

  async function act(operation: "stop" | "background") {
    if (!current) return;
    setBusy(true);
    try {
      await command("activity", session, {
        operation,
        activity_id: current.id || 0,
        agent_id: current.agent_id || "",
        text: "",
      });
    } catch (failure) {
      report(failure);
    } finally {
      setBusy(false);
    }
  }
  const live = !!current && !page && active(current);

  return (
    <Modal
      title={title}
      className="activity-view"
      layout="sheet"
      close={close}
      actions={
        live && (
          <>
            {current.kind !== "agent" && current.detached === false && (
              <IconButton
                label="Move to background"
                disabled={!online || busy}
                onClick={() => act("background")}
              >
                <ArrowDownToLine />
              </IconButton>
            )}
            <IconButton
              label={`Stop ${title}`}
              disabled={!online || busy || current.status === "stopping"}
              onClick={() => act("stop")}
            >
              <Square />
            </IconButton>
          </>
        )
      }
    >
      {page && (
        <div class="activity-detail activity-page">
          {detail && (
            <button
              type="button"
              class="quiet with-icon activity-back"
              onClick={() => setPage(null)}
            >
              <ArrowLeft aria-hidden="true" />
              Back
            </button>
          )}
          {page.raw ? (
            <Deferred
              load={rawDialog}
              fallback={<Spinner label="Loading full body…" surface />}
              {...page.raw}
            />
          ) : (
            detail && (
              <>
                {active(current || detail) && !detail.statistics_live && (
                  <p class="muted">
                    Totals reflect the latest saved checkpoint. Current work may
                    not yet be included.
                  </p>
                )}
                <Deferred
                  load={statisticsDialog}
                  fallback={<StatisticsLoading turn={!!page.stats?.blockId} />}
                  state={childStateOf(detail)}
                  blockId={page.stats?.blockId}
                />
              </>
            )
          )}
        </div>
      )}
      {detail && current ? (
        <DetailBody
          detail={detail}
          current={current}
          rows={rows}
          running={running}
          online={online}
          session={session}
          loading={loading}
          error={error}
          inspect={inspect}
          report={report}
          navigate={(next) => {
            // A newer page wins over any load still in flight.
            ++inspection.current;
            inspectionRequest.current?.abort();
            setDetail(next);
          }}
          page={setPage}
          loadDetail={loadDetail}
          hidden={!!page}
        />
      ) : (
        !page && <Spinner label="Loading…" surface />
      )}
    </Modal>
  );
}

function DetailBody({
  detail,
  current,
  rows,
  running,
  online,
  session,
  loading,
  error,
  inspect,
  report,
  navigate,
  page,
  loadDetail,
  hidden,
}: {
  // Hidden, not unmounted, while a page is shown: the draft and thread
  // scroll survive a visit to tool input/output or statistics.
  hidden: boolean;
  detail: ActivityDetail;
  current: Activity;
  rows: Activity[];
  running?: boolean;
  online: boolean;
  session: SessionRef;
  loading: boolean;
  error: unknown;
  inspect: (item: ActivityDetail, before?: number) => Promise<void>;
  report: Report;
  navigate: (detail: ActivityDetail) => void;
  page: (page: Page) => void;
  loadDetail: (
    item: ActivityDetail,
    before?: number,
    prior?: ActivityDetail | null,
    signal?: AbortSignal,
  ) => Promise<ActivityDetail>;
}) {
  const isAgent = !!detail.agent_id && !detail.memory;
  const isLive = active(current);
  const bare = !detail.conversation && !detail.output && !detail.task;
  // Thread follows the live edge exactly like the transcript (same
  // hook, keyed by agent): growth pins while sticky, any upward move
  // breaks, and the jump button below re-pins.
  const [following, setFollowing] = useState(true);
  const historyKey = `agent:${detail.agent_id || detail.id || "detail"}`;
  const thread = useTranscriptHistory(
    setFollowing,
    historyKey,
    detail.conversation?.blocks.length || 0,
  );
  // Guidance submit lives here (not the panel) so every nesting level
  // sends with its own text and receipt.
  const [text, setText] = useState("");
  const lifetime = useRef(new AbortController());
  useEffect(() => {
    lifetime.current = new AbortController();
    return () => lifetime.current.abort();
  }, [detail.agent_id, detail.id]);
  const [sending, setSending] = useState(false);
  const [notice, setNotice] = useState("");
  const [model, setModel] = useState("");
  const showTurnStats = useCallback(
    (block: Block) =>
      page({ title: "Subagent statistics", stats: { blockId: block.id } }),
    [page],
  );
  const childState = childStateOf(detail);
  useEffect(() => {
    setText("");
    setNotice("");
    setModel("");
  }, [detail.agent_id]);
  // One route stack keeps the thread in one stable surface. Nested agents use
  // the same history/rendering path and a Back action instead of recursive
  // dialogs with separate scroll and loading state.
  const [ancestors, setAncestors] = useState<ActivityDetail[]>([]);
  // Key scope and the default block reader follow the CHILD
  // conversation: stable child ids must never inherit parent row state.
  const childSession = useMemo(
    () => ({
      id: detail.agent_id || `activity-${detail.id ?? "detail"}`,
      generation: "",
    }),
    [detail.agent_id, detail.id],
  );
  const readDetail = useCallback(
    (id: string, rawFlag: boolean, signal = lifetime.current.signal) =>
      readPages(async (offset) => {
        const response = await command(
          "activity",
          session,
          {
            operation: "inspect",
            agent_id: detail.agent_id,
            detail: id,
            raw: rawFlag,
            offset,
          },
          { signal },
        );
        const body = response.pending ? undefined : response.result?.body;
        if (!body) throw new Error("Message is unavailable.");
        return body;
      }, signal),
    [session, detail.agent_id],
  );
  const openRaw = useCallback(
    async (id: string) => {
      const { signal } = lifetime.current;
      try {
        const body = await readDetail(id, true, signal);
        page({
          title: id.startsWith("t-") ? "Tool input/output" : "Full content",
          raw: {
            value: id.startsWith("t-")
              ? (JSON.parse(body.text) as JSONValue)
              : { output: body.text },
          },
        });
      } catch (failure) {
        if (!signal.aborted) report(failure);
      }
    },
    [readDetail, report, page],
  );
  const openNested = useCallback(
    async (block: Block) => {
      const { signal } = lifetime.current;
      try {
        const full = await loadDetail(
          {
            activity_id: block.activity_id,
            agent_id: block.agent_id,
            label: block.activity?.label || "Subagent",
            status: block.status,
            command: block.command,
            output: block.text,
          },
          undefined,
          undefined,
          signal,
        );
        signal.throwIfAborted();
        setAncestors((prior) => [...prior, detail]);
        navigate(full);
      } catch (failure) {
        if (!signal.aborted) report(failure);
      }
    },
    [detail, loadDetail, navigate, report],
  );
  const meta = [
    current.status,
    detail.route || detail.model,
    current.started_ms
      ? duration(
          Math.max(0, current.duration_ms ?? Date.now() - current.started_ms),
        )
      : "",
  ].filter(Boolean);

  return (
    <div class="activity-detail" hidden={hidden}>
      {ancestors.length > 0 && (
        <button
          type="button"
          class="quiet with-icon activity-back"
          onClick={() => {
            const parent = ancestors.at(-1);
            if (!parent) return;
            setAncestors((prior) => prior.slice(0, -1));
            navigate(parent);
          }}
        >
          <ArrowLeft aria-hidden="true" />
          Back to {ancestors.at(-1)?.name || "parent agent"}
        </button>
      )}
      {error && <LoadError error={error} retry={() => inspect(detail)} />}
      {loading && bare && !error && <Spinner label="Loading…" surface />}
      {meta.length > 0 && <p class="detail-meta">{meta.join(" · ")}</p>}

      {isAgent && (
        <details class="activity-info">
          <summary>
            Task and run details
            {!!detail.communication?.length &&
              ` · ${count(detail.communication.length)} messages`}
          </summary>
          {detail.description && (
            <section aria-label="About">
              <p class="muted">{cleanText(detail.description)}</p>
            </section>
          )}
          {detail.directive && (
            <section aria-label="Run details">
              <h3>Persistent directive</h3>
              <Markdown text={detail.directive} />
            </section>
          )}
          {!!detail.communication?.length && (
            <section aria-label="Agent communication" class="communication">
              <h3>Agent communication</h3>
              <ol>
                {detail.communication.map((message, index) => (
                  <li key={`${message.time}-${index}`}>
                    <div class="communication-meta">
                      <strong>
                        {rows.find((row) => row.agent_id === message.from)
                          ?.name || message.from}
                      </strong>
                      <span aria-hidden="true">→</span>
                      <strong>
                        {rows.find((row) => row.agent_id === message.to)
                          ?.name || message.to}
                      </strong>
                      <time dateTime={message.time}>{message.time}</time>
                    </div>
                    <p>{cleanText(message.text)}</p>
                  </li>
                ))}
              </ol>
            </section>
          )}
        </details>
      )}

      {!detail.conversation &&
        (detail.command ? (
          <section aria-label="Command">
            <pre class="detail-command" tabIndex={0}>
              {cleanText(detail.command)}
            </pre>
          </section>
        ) : (
          detail.label && <p class="detail-label">{detail.label}</p>
        ))}

      {!isAgent && !detail.conversation && detail.task && (
        <p class="detail-task">{detail.task}</p>
      )}

      {(isAgent || detail.conversation) && (
        <section aria-label="Thread">
          <div
            class="child-thread"
            data-history-key={historyKey}
            ref={thread.attachScroller}
            aria-label="Subagent task"
          >
            <div ref={thread.attachContent}>
              <HistoryStart
                view={detail.conversation}
                online={online}
                loading={loading}
                load={() =>
                  thread
                    .preserveWhile(() =>
                      inspect(detail, detail.conversation?.before),
                    )
                    .catch(report)
                }
              />
              {detail.conversation ? (
                <MessageRows
                  blocks={detail.conversation.blocks}
                  read={readDetail}
                  online={online}
                  session={childSession}
                  report={report}
                  inspect={openRaw}
                  activity={openNested}
                  statistics={showTurnStats}
                />
              ) : (
                <p class="muted">Waiting for the subagent transcript…</p>
              )}
              {isAgent && isLive && (
                <p
                  class="thread-progress"
                  title="Latest reported subagent activity"
                >
                  <ActivityStatus
                    phase={cleanText(
                      current.progress || detail.progress || "Working",
                    )}
                    running
                    announce
                  />
                </p>
              )}
            </div>
          </div>
          {!following && (
            <button
              type="button"
              class="jump quiet with-icon"
              onClick={() => {
                thread.jumpToLatest();
                inspect(detail).catch(report);
              }}
            >
              Jump to latest{" "}
              {thread.unseen > 0 && (
                <span aria-hidden="true">({thread.unseen} new)</span>
              )}
              <ArrowDown aria-hidden="true" />
            </button>
          )}
        </section>
      )}

      {detail.memory ? (
        <section aria-label="Memory">
          <p class="muted">
            Current memory · {detail.memory.key}. It may have changed since this
            event.
          </p>
          <Markdown text={detail.output || ""} />
        </section>
      ) : (
        !isAgent &&
        !detail.conversation && (
          <section aria-label="Output">
            {detail.output ? (
              <pre>{cleanText(detail.output)}</pre>
            ) : (
              !loading && <p class="muted">No output recorded.</p>
            )}
          </section>
        )
      )}

      {detail.conversation && detail.output && (
        <details>
          <summary>Process output</summary>
          <pre>{cleanText(detail.output)}</pre>
        </details>
      )}

      {isAgent && (
        <form
          class="guidance-form"
          onSubmit={async (event) => {
            event.preventDefault();
            if (!text.trim() || sending) return;
            setSending(true);
            try {
              const response = await command("activity", session, {
                operation: isLive ? "message" : "followup",
                activity_id: detail.id || 0,
                agent_id: detail.agent_id || "",
                text: text.trim(),
                ...(!isLive && model ? { model } : {}),
              });
              if (!response.pending) {
                setText("");
                setNotice(
                  isLive
                    ? "Guidance queued for the next step."
                    : "Follow-up started.",
                );
              }
            } catch (failure) {
              report(failure);
            } finally {
              setSending(false);
            }
          }}
        >
          <Field label={isLive ? "Guidance" : "Follow-up"}>
            <MessageInput
              submit={(event) =>
                (
                  event.currentTarget as HTMLTextAreaElement
                ).form?.requestSubmit()
              }
              value={text}
              rows={2}
              onInput={(event) => setText(event.currentTarget.value)}
            />
          </Field>
          <div class="composer-actions">
            <ModelControl
              session={session}
              state={childState}
              selection={model || childState.route}
              online={online}
              running={isLive || !!detail.persistent || sending}
              save={setModel}
            />
            <button
              type="submit"
              class="primary"
              disabled={
                !online || sending || !text.trim() || (!isLive && running)
              }
            >
              {sending
                ? "Sending…"
                : isLive
                  ? "Send guidance"
                  : "Start follow-up"}
            </button>
          </div>
          <div class="metrics">
            <ContextSummary state={childState} />
            <SessionSummary
              state={childState}
              open={() => page({ title: "Subagent statistics", stats: {} })}
            />
          </div>
          {detail.persistent && (
            <small class="muted">
              Persistent agents retain their model across follow-ups.
            </small>
          )}
          {notice && <p role="status">{notice}</p>}
        </form>
      )}
    </div>
  );
}
