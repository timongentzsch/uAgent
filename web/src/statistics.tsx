import { duration } from "./duration.ts";
import { count } from "./quantities.ts";
import "./statistics.css";
import type { JSX } from "preact";
import type { Usage, StatisticsModal, Snapshot } from "./types.ts";
import { useEffect, useState } from "preact/hooks";
import { command } from "./store.ts";
import { LoadError } from "./ui.tsx";
import { StatsSkeleton } from "./loading.tsx";

const rate = (value?: number) =>
  value !== undefined && Number.isFinite(value) && value > 0
    ? `${count(value)} tok/s`
    : "Not recorded";
const date = (value?: string) =>
  value && Number.isFinite(Date.parse(value))
    ? new Date(value).toLocaleString()
    : "Not recorded";
function Rows({ rows }: { rows: string[][] }) {
  return (
    <dl class="stats">
      {rows.map(([label, value]) => (
        <div key={label}>
          <dt>{label}</dt>
          <dd>{value}</dd>
        </div>
      ))}
    </dl>
  );
}
function UsageRows({ usage }: { usage?: Usage }) {
  return (
    <Rows
      rows={[
        ["Input tokens (uncached)", count(usage?.input)],
        ["Output tokens", count(usage?.output)],
        ["Reasoning tokens", count(usage?.reasoning)],
        ["Cache read tokens", count(usage?.cache_read)],
        ["Cache write tokens", count(usage?.cache_write)],
        [
          "Cost",
          usage?.cost_reported ? `$${usage.cost.toFixed(4)}` : "Not reported",
        ],
      ]}
    />
  );
}
export default function Statistics({
  modal,
  close,
  online,
  changed,
  loadSnapshot,
}: {
  modal: StatisticsModal;
  close: () => void;
  online: boolean;
  changed: (kind: string, id: string) => Promise<void>;
  loadSnapshot: (id: string) => Promise<Snapshot>;
}) {
  const [title, setTitle] = useState(modal.session?.title || "");
  const [busy, setBusy] = useState(false);
  const block = modal.block;
  const [snapshot, setSnapshot] = useState(modal.snapshot);
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  const state = snapshot?.state || {};
  const stats = state.statistics || {};
  const fork = state.view?.fork;
  const management = modal.type === "rename" || modal.type === "delete";
  useEffect(() => {
    if (management || block || snapshot) return;
    let active = true;
    setError(null);
    loadSnapshot(modal.session!.id)
      .then((value) => {
        if (active) setSnapshot(value);
      })
      .catch((failure) => {
        if (active) setError(failure);
      });
    return () => {
      active = false;
    };
  }, [attempt]);
  const save = async (event: JSX.TargetedSubmitEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (modal.type !== "rename" && modal.type !== "delete") return;
    setBusy(true);
    setError(null);
    try {
      await command(
        modal.type,
        modal.session,
        modal.type === "rename" ? { title: title.trim() } : {},
      );
      await changed(modal.type, modal.session.id);
      close();
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(false);
    }
  };
  if (!management && !block && !snapshot)
    return error ? (
      <LoadError error={error} retry={() => setAttempt(attempt + 1)} />
    ) : (
      <StatsSkeleton />
    );
  return (
    <>
      {error && <LoadError error={error} />}
      {modal.type === "rename" || modal.type === "delete" ? (
        <form onSubmit={save}>
          {modal.type === "rename" ? (
            <label>
              Conversation name
              <input
                value={title}
                onInput={(event) => setTitle(event.currentTarget.value)}
                required
                maxLength={256}
                autoComplete="off"
              />
            </label>
          ) : (
            <p>
              Delete “{modal.session.title}” and its saved messages and
              attachments? Project files stay in place. This cannot be undone.
            </p>
          )}
          <div class="dialog-actions">
            <button
              class="primary"
              disabled={
                !online || busy || (modal.type === "rename" && !title.trim())
              }
            >
              {modal.type === "rename" ? "Save name" : "Delete permanently"}
            </button>
            <button type="button" onClick={close}>
              Cancel
            </button>
          </div>
        </form>
      ) : (
        <>
          {!block && fork && (
            <p>
              Forked from “{fork.title}” after {count(fork.turns)} turns. Usage
              below counts work in this fork.
            </p>
          )}
          {block ? (
            <>
              <Rows
                rows={[
                  ["Timestamp", date(block.time)],
                  [
                    "Message",
                    block.kind === "assistant"
                      ? "Assistant response"
                      : block.kind === "tool_result"
                        ? "Tool result"
                        : "Submitted message",
                  ],
                  ...(block.kind === "assistant"
                    ? [
                        ["Model", block.route || "Not recorded"],
                        ["Duration", duration(block.duration_ms)],
                        ["TTFT", duration(block.ttft_ms)],
                        ["Throughput", rate(block.tokens_per_second)],
                      ]
                    : []),
                  ...(block.kind === "tool_result"
                    ? [
                        ["Tool", block.name || "Not recorded"],
                        ["Call ID", block.call_id || "Not recorded"],
                        ["Status", block.status || "Not recorded"],
                        ["Duration", duration(block.duration_ms)],
                      ]
                    : []),
                  ...(block.kind === "user" || block.kind === "attachment"
                    ? [
                        [
                          "Attachments",
                          count(
                            (block.images?.length || 0) +
                              (block.unavailable_images || 0),
                          ),
                        ],
                      ]
                    : []),
                ]}
              />
              {block.kind === "assistant" && (
                <UsageRows
                  usage={block.usage_reported ? block.usage : undefined}
                />
              )}
            </>
          ) : (
            <>
              <p>{snapshot?.metadata?.title}</p>
              <Rows
                rows={[
                  [
                    fork ? "Turns in fork" : "Turns",
                    count(
                      fork ? (state.turns ?? NaN) - fork.turns : state.turns,
                    ),
                  ],
                  [
                    "Tool calls",
                    count(stats.tool_calls ?? (stats.complete ? 0 : undefined)),
                  ],
                  [
                    "Model calls",
                    count(
                      stats.model_calls ?? (stats.complete ? 0 : undefined),
                    ),
                  ],
                  ["Turn time", duration(stats.duration_ms)],
                  ["Model time", duration(stats.model_ms)],
                  ["Tool time (summed)", duration(stats.tool_ms)],
                  [
                    "Mean TTFT",
                    duration(
                      stats.ttft_samples
                        ? (stats.ttft_ms ?? NaN) / stats.ttft_samples
                        : undefined,
                    ),
                  ],
                  ["TTFT samples", count(stats.ttft_samples)],
                  [
                    "Throughput",
                    rate(
                      stats.generation_ms
                        ? ((stats.generated_tokens ?? NaN) * 1000) /
                            stats.generation_ms
                        : undefined,
                    ),
                  ],
                ]}
              />
              <UsageRows
                usage={
                  stats.usage_samples ||
                  Object.values(state.usage || {}).some(
                    (value) => typeof value === "number" && value > 0,
                  )
                    ? state.usage
                    : undefined
                }
              />
              <p class="muted small">
                {stats.complete
                  ? "Totals through the last saved checkpoint, including compacted turns."
                  : "Older activity was not fully recorded. Timing and tool counters cover recorded activity only."}
              </p>
            </>
          )}
          {(!block || block.kind === "assistant") && (
            <p class="muted small">
              TTFT measures request start to first text or reasoning, including
              retries. Throughput is output plus reasoning tokens divided by
              total request time. Missing provider data stays unreported.
            </p>
          )}
        </>
      )}
    </>
  );
}
