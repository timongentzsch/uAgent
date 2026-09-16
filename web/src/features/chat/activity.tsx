import "./activity.css";
import { count } from "../../shared/quantities.ts";
import { duration } from "../../shared/duration.ts";
import type { Activity, ActivityDetail } from "../../shared/types.ts";
import type { ActivityProps } from "./activity-status.tsx";
import { useEffect, useRef, useState } from "preact/hooks";
import { Bot, Terminal, Square } from "lucide-preact";
import { command, readPages } from "../../state/api.ts";
import {
  cleanText,
  Field,
  Modal,
  Skeleton,
  LoadError,
  IconButton,
} from "../../shared/ui.tsx";

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
  const [text, setText] = useState("");
  const [busy, setBusy] = useState(false);
  const inspection = useRef(0);

  useEffect(
    () => () => {
      ++inspection.current;
    },
    [],
  );
  useEffect(() => setText(""), [detail?.agent_id]);

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
      if (item.memory) {
        const result = await manage("memory", {
          action: "get",
          cwd,
          key: item.memory.key,
        });
        if (version !== inspection.current) return;
        if (!result.item || result.item.error)
          throw new Error(
            result.item?.error || "Memory is no longer available.",
          );
        setDetail({ ...item, output: result.item.content || "" });
        return;
      }
      const response = await command("activity", session, {
        operation: "inspect",
        activity_id: item.id || item.activity_id || 0,
        agent_id: item.agent_id || "",
        ...(before ? { before } : {}),
      });
      if (version !== inspection.current) return;
      if (response.pending)
        throw new Error("Activity is still loading. Try again shortly.");
      setDetail((prior) => ({
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
      }));
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

  async function act(item: Activity, operation: string) {
    setBusy(true);
    try {
      const response = await command("activity", session, {
        operation,
        activity_id: item.id || 0,
        agent_id: item.agent_id || "",
        text,
      });
      if (["message", "followup"].includes(operation) && !response.pending) {
        setText("");
        setDetail((prior) => ({
          ...prior,
          receipt:
            operation === "message"
              ? "Guidance queued for the next step."
              : "Follow-up started.",
        }));
      }
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(false);
    }
  }

  const ordered = [...rows].sort(
    (a, b) => rank(a) - rank(b) || (a.started_ms || 0) - (b.started_ms || 0),
  );
  const past = ordered.filter(
    (item) => !active(item) && item.status !== "failed",
  );
  const visible = ordered.filter(
    (item) => showPast || active(item) || item.status === "failed",
  );
  const close = () => {
    ++inspection.current;
    setDetail(null);
  };

  return (
    <div class="activity-panel">
      <ul class="activity-list">
        {!rows.length && (
          <li class="activity-empty muted">No background activity.</li>
        )}
        {visible.map((item) => (
          <li class="activity-row" key={key(item)}>
            <button
              type="button"
              class="quiet activity-open"
              disabled={!online}
              onClick={() => inspect(item).catch(report)}
            >
              {item.kind === "agent" ? <Bot /> : <Terminal />}
              <span class="activity-text">
                <strong>{item.label}</strong>
                <small>
                  {item.status}
                  {(item.progress || item.model) &&
                    ` · ${cleanText(item.progress || item.model || "")}`}
                </small>
              </span>
              {item.started_ms && (
                <time>
                  {duration(
                    Math.max(0, item.duration_ms ?? now - item.started_ms),
                  )}
                </time>
              )}
            </button>
            {active(item) && (
              <IconButton
                label={`Stop ${item.label}`}
                disabled={!online || busy || item.status === "stopping"}
                onClick={() => act(item, "stop")}
              >
                <Square />
              </IconButton>
            )}
          </li>
        ))}
      </ul>
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
          loading={loading}
          busy={busy}
          error={error}
          text={text}
          setText={setText}
          inspect={inspect}
          act={act}
          report={report}
          close={close}
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
  loading,
  busy,
  error,
  text,
  setText,
  inspect,
  act,
  report,
  close,
}: {
  detail: ActivityDetail;
  rows: Activity[];
  running?: boolean;
  online: boolean;
  session: ActivityProps["session"];
  loading: boolean;
  busy: boolean;
  error: unknown;
  text: string;
  setText: (value: string) => void;
  inspect: (item: ActivityDetail, before?: number) => Promise<void>;
  act: (item: Activity, operation: string) => Promise<void>;
  report: ActivityProps["report"];
  close: () => void;
}) {
  const current =
    rows.find((item) =>
      detail.agent_id
        ? item.agent_id === detail.agent_id
        : item.id === detail.id,
    ) || detail;
  const isAgent = !!detail.agent_id && !detail.memory;
  const isLive = active(current);
  const title = detail.memory ? "Memory" : isAgent ? "Subagent" : "Activity";
  const bare = !detail.conversation && !detail.output && !detail.task;
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
            <div class="child-thread" aria-label="Subagent task">
              <MessageRows
                blocks={detail.conversation.blocks}
                read={(id, raw, signal) =>
                  readPages(async (offset) => {
                    const response = await command("activity", session, {
                      operation: "inspect",
                      agent_id: detail.agent_id,
                      detail: id,
                      raw,
                      offset,
                    });
                    const body = response.pending
                      ? undefined
                      : response.result?.body;
                    if (!body) throw new Error("Message is unavailable.");
                    return body;
                  }, signal)
                }
                online={online}
                session={session}
                report={report}
              />
            </div>
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
            onSubmit={(event) => {
              event.preventDefault();
              act(detail, isLive ? "message" : "followup");
            }}
          >
            <Field label={isLive ? "Guidance" : "Follow-up"}>
              <textarea
                value={text}
                rows={2}
                onInput={(event) => setText(event.currentTarget.value)}
              />
            </Field>
            <div class="dialog-actions">
              <button
                type="submit"
                class="primary"
                disabled={
                  !online || busy || !text.trim() || (!isLive && running)
                }
              >
                {busy
                  ? "Sending…"
                  : isLive
                    ? "Send guidance"
                    : "Start follow-up"}
              </button>
            </div>
            {detail.receipt && <p role="status">{detail.receipt}</p>}
          </form>
        )}
      </div>
    </Modal>
  );
}
