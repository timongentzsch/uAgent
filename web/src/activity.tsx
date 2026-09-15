import "./activity.css";
import { count } from "./quantities.ts";
import { duration } from "./duration.ts";
import type { Activity, ActivityDetail } from "./types.ts";
import type { ActivityProps } from "./activity-status.tsx";
import { useEffect, useRef, useState } from "preact/hooks";
import { Bot, Terminal, Square } from "lucide-preact";
import { command, readPages } from "./api.ts";
import {
  cleanText,
  Field,
  Modal,
  Skeleton,
  LoadError,
  IconButton,
} from "./ui.tsx";

import { active } from "./activity-status.tsx";
import { manage } from "./management.tsx";
import Markdown from "./markdown-view.tsx";
import { MessageRows } from "./message.tsx";
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
  const [completed, setCompleted] = useState(false);
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
    } catch (error) {
      if (version === inspection.current) setError(error);
    } finally {
      if (version === inspection.current) setLoading(false);
    }
  }
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
      // The full command travels on the transcript block (bounded 8k);
      // rows abbreviate to 160 chars, so the recorded popup must prefer
      // the block when the supervisor no longer retains the job.
      command:
        (target as unknown as { command?: string }).command ||
        (item as unknown as { command?: string } | undefined)?.command,
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
  useEffect(() => {
    if (!detail) return;
    const item = rows.find((item) =>
      detail.agent_id
        ? item.agent_id === detail.agent_id
        : item.id === detail.id,
    );
    if (!item) return;
    // Coalesce output events while the detail is open; no status polling.
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
  const ordered = rows.sort((a, b) => {
    const rank = (item: Activity) =>
      item.status === "failed" ? 0 : active(item) ? 1 : 2;
    return rank(a) - rank(b) || (a.started_ms || 0) - (b.started_ms || 0);
  });
  const past = ordered.filter(
    (item) => !active(item) && item.status !== "failed",
  );
  const currentDetail =
    rows.find(
      (item) => detail?.agent_id && item.agent_id === detail.agent_id,
    ) ||
    rows.find((item) => item.id && item.id === detail?.id) ||
    detail;
  return (
    <div class="activity-panel">
      <div class="activity-list">
        {!rows.length && <p class="muted">No background activity.</p>}
        {ordered
          .filter(
            (item) => completed || active(item) || item.status === "failed",
          )
          .map((item) => (
            <div class="activity-row" key={item.id || item.agent_id}>
              <button
                type="button"
                class="quiet"
                disabled={!online || busy}
                onClick={() => inspect(item).catch(report)}
              >
                {item.kind === "agent" ? <Bot /> : <Terminal />}
                <span>
                  <strong>{item.label}</strong>
                  <small>
                    {item.status} ·{" "}
                    {cleanText(item.progress || item.model || "")}
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
            </div>
          ))}
        {past.length > 0 && (
          <button
            type="button"
            class="quiet"
            onClick={() => setCompleted(!completed)}
          >
            {completed ? "Hide" : "Show"} {count(past.length)} completed / idle
          </button>
        )}
      </div>
      {detail && (
        <Modal
          title={
            detail.memory
              ? "Memory"
              : detail.command
                ? "Activity"
                : detail.agent_id
                  ? "Subagent"
                  : "Activity"
          }
          className="activity-view"
          close={() => {
            ++inspection.current;
            setDetail(null);
          }}
        >
          {error && <LoadError error={error} retry={() => inspect(detail)} />}
          {loading && <Skeleton label="Loading activity…" />}
          {/* Lean header: full command scrollable on top, then the task.
              Labels repeat the modal title, so they only show for plain
              command records without a thread. */}
          {!detail.conversation && detail.label && (
            <p class="detail-label">{detail.label}</p>
          )}
          <p class="muted">
            {currentDetail?.status}
            {detail.model && ` · ${detail.model}`}
          </p>
          {(detail.command || target?.command) && (
            <pre class="detail-command" tabIndex={0}>
              {cleanText(detail.command || target?.command || "")}
            </pre>
          )}
          {detail.task && <p class="detail-task">{detail.task}</p>}
          {detail.directive && (
            <details>
              <summary>Persistent directive</summary>
              <Markdown text={detail.directive} />
            </details>
          )}
          {detail.system_prompt && (
            <details class="prompt-disclosure">
              <summary>System prompt</summary>
              <pre>{detail.system_prompt}</pre>
            </details>
          )}
          {detail.memory && (
            <p class="muted">
              Current memory · {detail.memory.key}. It may have changed since
              this event.
            </p>
          )}
          {detail.statistics && (
            <details>
              <summary>Run statistics</summary>
              <p class="muted">
                {count(detail.turns)} turns ·{" "}
                {count(detail.statistics.model_calls)} model calls ·{" "}
                {count(detail.statistics.tool_calls)} tool calls ·{" "}
                {count(detail.usage?.output)} output tokens
              </p>
            </details>
          )}
          {detail.conversation ? (
            <>
              {detail.conversation.more && (
                <button
                  onClick={() =>
                    inspect(detail, detail.conversation?.before).catch(report)
                  }
                >
                  Load older child messages
                </button>
              )}
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
            </>
          ) : detail.memory ? (
            <Markdown text={detail.output || ""} />
          ) : (
            <pre>
              {cleanText(
                detail.output || (loading ? "" : "No output recorded."),
              )}
            </pre>
          )}
          {detail.conversation && detail.output && (
            <details>
              <summary>Process output</summary>
              <pre>{cleanText(detail.output)}</pre>
            </details>
          )}
          {detail.agent_id && (
            <>
              <Field
                label={
                  active(currentDetail || detail) ? "Guidance" : "Follow-up"
                }
              >
                <textarea
                  value={text}
                  onInput={(event) => setText(event.currentTarget.value)}
                />
              </Field>
              <button
                disabled={
                  !online ||
                  busy ||
                  !text.trim() ||
                  (!active(currentDetail || detail) && running)
                }
                onClick={() =>
                  act(
                    detail,
                    active(currentDetail || detail) ? "message" : "followup",
                  )
                }
              >
                {active(currentDetail || detail)
                  ? "Send guidance"
                  : "Start follow-up"}
              </button>
              {detail.receipt && <p role="status">{detail.receipt}</p>}
            </>
          )}
        </Modal>
      )}
    </div>
  );
}
