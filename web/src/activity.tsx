import "./activity.css";
import { count } from "./quantities.ts";
import { duration } from "./duration.ts";
import type {
  Activity,
  ActivityDetail,
  Collaborator,
  SessionRef,
  Report,
  Block,
} from "./types.ts";
export interface ActivityProps {
  items?: Activity[];
  collaborators?: Collaborator[];
  target?: Block | null;
  clearTarget: () => void;
  running?: boolean;
  session: SessionRef;
  online: boolean;
  report: Report;
}
import { useEffect, useRef, useState } from "preact/hooks";
import { Bot, Terminal, Square } from "lucide-preact";
import { command } from "./store.ts";
import { cleanText, Modal, Skeleton, LoadError } from "./ui.tsx";

import { active } from "./activity-status.tsx";
export default function Activities({
  items = [],
  collaborators = [],
  target,
  clearTarget,
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
        status: "idle",
      })),
  ];
  const live = items.some(active);
  useEffect(() => {
    if (!live) return;
    const timer = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(timer);
  }, [live]); // Display clock only; runtime updates arrive through SSE.
  async function inspect(item: Activity, before?: number) {
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
    inspect({ activity_id: target.activity_id, agent_id: target.agent_id });
    clearTarget();
  }, [target]);
  useEffect(() => {
    if (!detail) return;
    const item = items.find((item) => item.id === detail.id);
    if (!item) return;
    // Coalesce output events while the detail is open; no status polling.
    const timer = setTimeout(() => inspect(item).catch(report), 150);
    return () => clearTimeout(timer);
  }, [items, detail?.id]);
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
    items.find(
      (item) =>
        detail?.agent_id && item.agent_id === detail.agent_id && active(item),
    ) ||
    items.find((item) => item.id && item.id === detail?.id) ||
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
                <button
                  type="button"
                  class="quiet"
                  disabled={!online || busy || item.status === "stopping"}
                  aria-label={`Stop ${item.label}`}
                  onClick={() => act(item, "stop")}
                >
                  <Square />
                </button>
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
          title={detail.label || "Activity"}
          className="activity-view"
          close={() => {
            ++inspection.current;
            setDetail(null);
          }}
        >
          {error && <LoadError error={error} retry={() => inspect(detail)} />}
          {loading && <Skeleton label="Loading activity…" />}
          <p>
            {currentDetail?.status} · {detail.model || detail.kind}
          </p>
          {detail.statistics && (
            <p class="muted">
              {count(detail.turns)} turns ·{" "}
              {count(detail.statistics.model_calls)} model calls ·{" "}
              {count(detail.statistics.tool_calls)} tool calls ·{" "}
              {count(detail.usage?.output)} output tokens
            </p>
          )}
          {detail.conversation ? (
            <>
              <h3>Child conversation</h3>
              {detail.conversation.more && (
                <button
                  onClick={() =>
                    inspect(detail, detail.conversation?.before).catch(report)
                  }
                >
                  Load older child messages
                </button>
              )}
              {detail.conversation.blocks.map((block) => (
                <details
                  key={block.id}
                  open={
                    ["user", "assistant"].includes(block.kind) &&
                    !block.tools?.length
                  }
                >
                  <summary>
                    {block.kind}{" "}
                    {block.time &&
                      `· ${new Date(block.time).toLocaleTimeString()}`}
                    {block.route && ` · ${block.route}`}
                  </summary>
                  <pre>
                    {cleanText(
                      block.text || JSON.stringify(block.tools, null, 2),
                    )}
                  </pre>
                </details>
              ))}
            </>
          ) : (
            <pre>{cleanText(detail.output || "No output yet.")}</pre>
          )}
          {detail.conversation && detail.output && (
            <details>
              <summary>Process output</summary>
              <pre>{cleanText(detail.output)}</pre>
            </details>
          )}
          {detail.agent_id && (
            <>
              <label>
                {active(currentDetail || detail) ? "Guidance" : "Follow-up"}
                <textarea
                  value={text}
                  onInput={(event) => setText(event.currentTarget.value)}
                />
              </label>
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
