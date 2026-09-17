import "./activity.css";
import { count } from "../../shared/quantities.ts";
import { duration } from "../../shared/duration.ts";
import type {
  Activity,
  ActivityDetail,
  Block,
  JSONValue,
} from "../../shared/types.ts";
import type { ActivityProps } from "./activity-status.tsx";
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import { Bot, Terminal, Square, ArrowDown } from "lucide-preact";
import { command, readPages } from "../../state/api.ts";
import { useStickToBottom } from "../../state/use-stick-to-bottom.ts";
import {
  cleanText,
  Deferred,
  Field,
  Modal,
  Skeleton,
  LoadError,
  IconButton,
} from "../../shared/ui.tsx";
import { RawSkeleton } from "../../shared/loading.tsx";

import { active } from "./activity-status.tsx";
import { manage } from "../settings/management.tsx";
import Markdown from "../../shared/markdown-view.tsx";
import { MessageRows } from "./message.tsx";

function key(item: Activity): string {
  return String(item.id ?? item.agent_id ?? item.label);
}

function rank(item: Activity): number {
  if (item.status === "failed") return 0;
  if (active(item)) return 1;
  return 2;
}

// Pure fetch half of an activity inspect: memory content or the full
// agent/activity detail, with older-page appends merged. State (version
// guards, loading, errors) stays with the caller so panel and nested
// viewers share the loader without sharing UI state.
async function fetchActivityDetail(
  session: ActivityProps["session"],
  cwd: string,
  item: ActivityDetail,
  before?: number,
  prior?: ActivityDetail | null,
): Promise<ActivityDetail> {
  if (item.memory) {
    const result = await manage("memory", {
      action: "get",
      cwd,
      key: item.memory.key,
    });
    if (!result.item || result.item.error)
      throw new Error(result.item?.error || "Memory is no longer available.");
    return { ...item, output: result.item.content || "" };
  }
  const response = await command("activity", session, {
    operation: "inspect",
    activity_id: item.id || item.activity_id || 0,
    agent_id: item.agent_id || "",
    ...(before ? { before } : {}),
  });
  if (response.pending)
    throw new Error("Activity is still loading. Try again shortly.");
  return {
    ...item,
    ...response.result,
    ...(before && prior?.conversation
      ? {
          conversation: {
            ...response.result.conversation,
            blocks: [
              ...(response.result.conversation?.blocks || []),
              ...prior.conversation.blocks,
            ].slice(0, 256),
          },
        }
      : {}),
  };
}

const rawDialog = () => import("../settings/raw.tsx");

export default function Activities({
  items = [],
  collaborators = [],
  target,
  clearTarget,
  cwd,
  running,
  session,
  online,
  report,
}: ActivityProps) {
  const [now, setNow] = useState(Date.now());
  const [showPast, setShowPast] = useState(false);
  const [detail, setDetail] = useState<ActivityDetail | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const inspection = useRef(0);

  useEffect(
    () => () => {
      ++inspection.current;
    },
    [],
  );

  // Bound loader shared with nested viewers (which own their UI state).
  const loadDetail = useCallback(
    (item: ActivityDetail, before?: number, prior?: ActivityDetail | null) =>
      fetchActivityDetail(session, cwd, item, before, prior),
    [session, cwd],
  );

  const rows: Activity[] = [
    ...items,
    ...collaborators
      .filter((child) => !items.some((item) => item.agent_id === child.id))
      .map((child) => ({
        ...child,
        id: undefined,
        agent_id: child.id,
        kind: "agent",
        status: child.status || "idle",
      })),
  ];
  const live = rows.some(active);
  useEffect(() => {
    if (!live) return;
    const timer = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(timer);
  }, [live]); // Display clock only; runtime updates arrive through SSE.

  async function inspect(item: ActivityDetail, before?: number) {
    const version = ++inspection.current;
    if (!before)
      setDetail((prior) =>
        prior?.id === item.id && prior?.agent_id === item.agent_id
          ? prior
          : item,
      );
    setLoading(true);
    setError(null);
    try {
      const next = await loadDetail(item, before, before ? detail : undefined);
      if (version !== inspection.current) return;
      setDetail(next);
    } catch (failure) {
      if (version === inspection.current) setError(failure);
    } finally {
      if (version === inspection.current) setLoading(false);
    }
  }

  // Open a transcript receipt in the modal. The full command travels on the
  // transcript block (bounded 8k); rows abbreviate to 160 chars, so the
  // recorded popup prefers the block when the supervisor no longer retains
  // the job.
  useEffect(() => {
    if (!target) return;
    const item = items.find((item) => item.id === target.activity_id);
    const receipt = {
      ...item,
      activity_id: target.activity_id,
      agent_id: target.agent_id,
      label: target.activity?.label || "Recorded event",
      status: target.status,
      memory: target.memory,
      command:
        (target as unknown as { command?: string }).command || item?.command,
      output: target.text,
    };
    if (item || target.agent_id || target.memory) inspect(receipt);
    else {
      ++inspection.current;
      setDetail(receipt);
      setError(null);
      setLoading(false);
    }
    clearTarget();
  }, [target]);

  // Coalesce output events while the detail is open; no status polling.
  // Finished records are immutable: re-inspecting them on every list
  // change churns the open popup (and can flash errors over settled
  // output). Live rows keep coalesced refreshes while they run.
  useEffect(() => {
    if (!detail) return;
    const item = rows.find((item) =>
      detail.agent_id
        ? item.agent_id === detail.agent_id
        : item.id === detail.id,
    );
    if (!item) return;
    if (!active(item)) return;
    const timer = setTimeout(() => inspect(item).catch(report), 150);
    return () => clearTimeout(timer);
  }, [items, collaborators, detail?.id, detail?.agent_id]);

  // Stop-only: guidance/follow-up submit lives in the modal (per-level
  // text and receipts), so this never touches message state.
  async function act(item: Activity, operation: string) {
    setBusy(true);
    try {
      await command("activity", session, {
        operation,
        activity_id: item.id || 0,
        agent_id: item.agent_id || "",
        text: "",
      });
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(false);
    }
  }

  const ordered = [...rows].sort(
    (a, b) => rank(a) - rank(b) || (a.started_ms || 0) - (b.started_ms || 0),
  );
  const isAgentRow = (item: Activity) => item.kind === "agent";
  const agentName = (item: Activity) => item.name || item.label;
  const visibleAgents = ordered.filter(
    (item) =>
      isAgentRow(item) &&
      (showPast || active(item) || item.status === "failed"),
  );
  const visibleTasks = ordered.filter(
    (item) =>
      !isAgentRow(item) &&
      (showPast || active(item) || item.status === "failed"),
  );
  const past = ordered.filter(
    (item) => !active(item) && item.status !== "failed",
  );
  const close = () => {
    ++inspection.current;
    setDetail(null);
  };

  const renderRow = (item: Activity) => (
    <li class="activity-row" key={key(item)}>
      <button
        type="button"
        class="quiet activity-open"
        disabled={!online}
        onClick={() => inspect(item).catch(report)}
      >
        {item.kind === "agent" ? <Bot /> : <Terminal />}
        <span class="activity-text">
          <strong>{isAgentRow(item) ? agentName(item) : item.label}</strong>
          <small>
            {item.status}
            {(item.progress || item.model) &&
              ` · ${cleanText(item.progress || item.model || "")}`}
          </small>
          {isAgentRow(item) && item.description && (
            <small class="muted">{cleanText(item.description)}</small>
          )}
        </span>
        {item.started_ms && (
          <time>
            {duration(Math.max(0, item.duration_ms ?? now - item.started_ms))}
          </time>
        )}
      </button>
      {active(item) && (
        <IconButton
          label={`Stop ${isAgentRow(item) ? agentName(item) : item.label}`}
          disabled={!online || busy || item.status === "stopping"}
          onClick={() => act(item, "stop")}
        >
          <Square />
        </IconButton>
      )}
    </li>
  );

  return (
    <div class="activity-panel">
      {!rows.length && (
        <p class="activity-empty muted">No background activity.</p>
      )}
      {visibleAgents.length > 0 && (
        <section aria-label="Subagents">
          <h3 class="activity-heading">Subagents ({visibleAgents.length})</h3>
          <ul class="activity-list">{visibleAgents.map(renderRow)}</ul>
        </section>
      )}
      {visibleTasks.length > 0 && (
        <section aria-label="Background tasks">
          <h3 class="activity-heading">
            Background tasks ({visibleTasks.length})
          </h3>
          <ul class="activity-list">{visibleTasks.map(renderRow)}</ul>
        </section>
      )}
      {past.length > 0 && (
        <button
          type="button"
          class="quiet activity-past-toggle"
          onClick={() => setShowPast(!showPast)}
        >
          {showPast ? "Hide" : "Show"} {count(past.length)} completed / idle
        </button>
      )}
      {detail && (
        <ActivityModal
          detail={detail}
          rows={rows}
          running={running}
          online={online}
          session={session}
          cwd={cwd}
          loading={loading}
          error={error}
          inspect={inspect}
          report={report}
          close={close}
          loadDetail={loadDetail}
        />
      )}
    </div>
  );
}

function ActivityModal({
  detail,
  rows,
  running,
  online,
  session,
  cwd,
  loading,
  error,
  inspect,
  report,
  close,
  loadDetail,
}: {
  detail: ActivityDetail;
  rows: Activity[];
  running?: boolean;
  online: boolean;
  session: ActivityProps["session"];
  cwd: string;
  loading: boolean;
  error: unknown;
  inspect: (item: ActivityDetail, before?: number) => Promise<void>;
  report: ActivityProps["report"];
  close: () => void;
  loadDetail: (
    item: ActivityDetail,
    before?: number,
    prior?: ActivityDetail | null,
  ) => Promise<ActivityDetail>;
}) {
  const current =
    rows.find((item) =>
      detail.agent_id
        ? item.agent_id === detail.agent_id
        : item.id === detail.id,
    ) || detail;
  const isAgent = !!detail.agent_id && !detail.memory;
  const isLive = active(current);
  const title = detail.memory
    ? "Memory"
    : isAgent
      ? detail.name || "Subagent"
      : "Activity";
  const bare = !detail.conversation && !detail.output && !detail.task;
  // Thread follows the live edge exactly like the transcript (same
  // hook, keyed by agent): growth pins while sticky, any upward move
  // breaks, and the jump button below re-pins.
  const [following, setFollowing] = useState(true);
  const thread = useStickToBottom(
    setFollowing,
    `agent:${detail.agent_id || detail.id || "detail"}`,
    detail.conversation?.blocks.length || 0,
  );
  // Guidance submit lives here (not the panel) so every nesting level
  // sends with its own text and receipt.
  const [text, setText] = useState("");
  const [sending, setSending] = useState(false);
  const [notice, setNotice] = useState("");
  useEffect(() => {
    setText("");
    setNotice("");
  }, [detail.agent_id]);
  // Nested tool viewer + nested subagent: own dialogs stacked on this
  // one, so the parent thread keeps its state underneath.
  const [raw, setRaw] = useState<{ title: string; value: JSONValue } | null>(
    null,
  );
  const [child, setChild] = useState<ActivityDetail | null>(null);
  const childVersion = useRef(0);
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
    (id: string, rawFlag: boolean, signal?: AbortSignal) =>
      readPages(async (offset) => {
        const response = await command("activity", session, {
          operation: "inspect",
          agent_id: detail.agent_id,
          detail: id,
          raw: rawFlag,
          offset,
        });
        const body = response.pending ? undefined : response.result?.body;
        if (!body) throw new Error("Message is unavailable.");
        return body;
      }, signal),
    [session, detail.agent_id],
  );
  const openRaw = useCallback(
    async (id: string) => {
      try {
        const page = await readDetail(id, true);
        setRaw({
          title: id.startsWith("t-") ? "Tool input/output" : "Full content",
          value: id.startsWith("t-")
            ? (JSON.parse(page.text) as JSONValue)
            : { output: page.text },
        });
      } catch (failure) {
        report(failure);
      }
    },
    [readDetail, report],
  );
  const openNested = useCallback(
    async (block: Block) => {
      const version = ++childVersion.current;
      try {
        const full = await loadDetail({
          activity_id: block.activity_id,
          agent_id: block.agent_id,
          label: block.activity?.label || "Subagent",
          status: block.status,
          command: (block as unknown as { command?: string }).command,
          output: block.text,
        });
        if (version === childVersion.current) setChild(full);
      } catch (failure) {
        if (version === childVersion.current) report(failure);
      }
    },
    [loadDetail, report],
  );
  const inspectChild = useCallback(
    async (item: ActivityDetail, before?: number) => {
      const version = ++childVersion.current;
      const full = await loadDetail(item, before, before ? child : undefined);
      if (version === childVersion.current) setChild(full);
    },
    [loadDetail, child],
  );
  // Live nested rows refresh like the top level (panel owns the top).
  useEffect(() => {
    if (!child) return;
    const item = rows.find((row) =>
      child.agent_id ? row.agent_id === child.agent_id : row.id === child.id,
    );
    if (!item || !active(item)) return;
    const timer = setTimeout(() => inspectChild(child).catch(report), 150);
    return () => clearTimeout(timer);
  }, [rows, child?.id, child?.agent_id]);
  const meta = [
    current.status,
    detail.model,
    current.started_ms
      ? duration(
          Math.max(0, current.duration_ms ?? Date.now() - current.started_ms),
        )
      : "",
  ].filter(Boolean);

  return (
    <Modal title={title} className="activity-view" close={close}>
      <div class="activity-detail">
        {error && <LoadError error={error} retry={() => inspect(detail)} />}
        {loading && bare && !error && (
          <Skeleton label={`Loading ${title.toLowerCase()}…`} />
        )}
        {meta.length > 0 && <p class="detail-meta">{meta.join(" · ")}</p>}

        {isAgent && detail.description && (
          <section aria-label="About">
            <p class="muted">{cleanText(detail.description)}</p>
          </section>
        )}

        {(detail.directive || detail.statistics) && (
          <section aria-label="Run details">
            {detail.directive && (
              <>
                <h3>Persistent directive</h3>
                <Markdown text={detail.directive} />
              </>
            )}
            {detail.statistics && (
              <p class="muted">
                {count(detail.turns)} turns ·{" "}
                {count(detail.statistics.model_calls)} model calls ·{" "}
                {count(detail.statistics.tool_calls)} tool calls ·{" "}
                {count(detail.usage?.output)} output tokens
              </p>
            )}
          </section>
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

        {!detail.conversation && detail.task && (
          <p class="detail-task">{detail.task}</p>
        )}

        {detail.conversation && (
          <section aria-label="Thread">
            <div
              class="child-thread"
              ref={thread.attachScroller}
              aria-label="Subagent task"
            >
              <div ref={thread.attachContent}>
                <MessageRows
                  blocks={detail.conversation.blocks}
                  read={readDetail}
                  online={online}
                  session={childSession}
                  report={report}
                  inspect={openRaw}
                  activity={openNested}
                />
              </div>
            </div>
            {!following && (
              <button
                type="button"
                class="jump quiet with-icon"
                onClick={() => thread.jumpToLatest()}
              >
                Jump to latest{" "}
                {thread.unseen > 0 && (
                  <span aria-hidden="true">({thread.unseen} new)</span>
                )}
                <ArrowDown aria-hidden="true" />
              </button>
            )}
            {detail.conversation.more && (
              <button
                type="button"
                class="quiet history-button"
                disabled={!online || loading}
                aria-busy={loading || undefined}
                onClick={() =>
                  inspect(detail, detail.conversation?.before).catch(report)
                }
              >
                {loading ? "Loading older messages…" : "Load older messages"}
              </button>
            )}
          </section>
        )}

        {detail.memory ? (
          <section aria-label="Memory">
            <p class="muted">
              Current memory · {detail.memory.key}. It may have changed since
              this event.
            </p>
            <Markdown text={detail.output || ""} />
          </section>
        ) : (
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
              <textarea
                value={text}
                rows={2}
                onInput={(event) => setText(event.currentTarget.value)}
                onKeyDown={(event) => {
                  if (
                    (event.metaKey || event.ctrlKey) &&
                    event.key === "Enter"
                  ) {
                    event.currentTarget.form?.requestSubmit();
                  }
                }}
              />
            </Field>
            <div class="dialog-actions">
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
            {notice && <p role="status">{notice}</p>}
          </form>
        )}
      </div>
      {raw && (
        <Modal title={raw.title} close={() => setRaw(null)}>
          <Deferred
            load={rawDialog}
            fallback={<RawSkeleton />}
            value={raw.value}
          />
        </Modal>
      )}
      {child && (
        <ActivityModal
          detail={child}
          rows={rows}
          running={running}
          online={online}
          session={session}
          cwd={cwd}
          loading={false}
          error={null}
          inspect={inspectChild}
          report={report}
          close={() => setChild(null)}
          loadDetail={loadDetail}
        />
      )}
    </Modal>
  );
}
