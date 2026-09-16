import { duration } from "./duration.ts";
import { count } from "./quantities.ts";
import "./statistics.css";
import type { Usage, StatisticsModal, Snapshot } from "./types.ts";
import { useEffect, useState } from "preact/hooks";
import { LoadError } from "./ui.tsx";
import { StatsSkeleton } from "./loading.tsx";
import { presentMessages } from "./message-view.ts";

const rate = (value?: number) =>
  value && value > 0 ? `${count(value)} tok/s` : "Not recorded";
function Rows({ rows }: { rows: [string, string][] }) {
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
  loadSnapshot,
}: {
  modal: Extract<StatisticsModal, { type: "statistics" }>;
  loadSnapshot: (id: string) => Promise<Snapshot>;
}) {
  const [snapshot, setSnapshot] = useState<Snapshot>();
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  const [scope, setScope] = useState(modal.block_id ? "turn" : "session");
  const presented = snapshot?.state?.view
    ? presentMessages(snapshot.state.view.blocks)
    : [];
  const flattened = presented.flatMap((row) =>
    row.children ? [row, ...row.children] : [row],
  );
  const block =
    scope === "turn"
      ? flattened.find(
          (row) =>
            row.key === modal.block_id ||
            row.id === modal.block_id ||
            row.response_id === modal.block_id ||
            row.occurrence_id === modal.block_id,
        )
      : undefined;
  useEffect(() => {
    let active = true;
    setSnapshot(undefined);
    setError(null);
    loadSnapshot(modal.session_id)
      .then((value) => {
        if (active) setSnapshot(value);
      })
      .catch((error) => {
        if (active) setError(error);
      });
    return () => {
      active = false;
    };
  }, [modal.session_id, attempt]);
  const state = snapshot?.state;
  const stats = state?.statistics;
  const summary = block?.summary;
  const fork = state?.view?.fork;
  const rows: [string, string][] = summary
    ? [
        ["Outcome", summary.outcome],
        ["Model at completion", summary.route || "Not recorded"],
        ["Model steps", count(summary.steps)],
        ["Tool calls", count(summary.tool_calls)],
        ["Duration", duration(summary.duration_ms)],
        ["TTFT", duration(summary.ttt_ms)],
        ["Throughput", rate(summary.tokens_per_second)],
      ]
    : block
      ? [
          [
            "Timestamp",
            block.time ? new Date(block.time).toLocaleString() : "Not recorded",
          ],
          ["Model", block.route || "Not recorded"],
          ["Tool", block.name || "—"],
          ["Status", block.status || "—"],
          ["Duration", duration(block.duration_ms)],
          ["TTFT", duration(block.ttft_ms)],
          ["Throughput", rate(block.tokens_per_second)],
        ]
      : [
          [
            fork ? "Turns in fork" : "Turns",
            count(
              stats?.recorded_turns ??
                (state?.turns === undefined
                  ? undefined
                  : state.turns - (fork?.turns || 0)),
            ),
          ],
          [
            "Tool calls",
            count(stats?.tool_calls ?? (stats?.complete ? 0 : undefined)),
          ],
          [
            "Model calls",
            count(stats?.model_calls ?? (stats?.complete ? 0 : undefined)),
          ],
          ["Turn time", duration(stats?.duration_ms)],
          ["Model time", duration(stats?.model_ms)],
          ["Tool time (summed)", duration(stats?.tool_ms)],
          [
            "Mean TTFT",
            duration(
              stats?.ttft_samples
                ? (stats.ttft_ms ?? NaN) / stats.ttft_samples
                : undefined,
            ),
          ],
          [
            "Throughput",
            rate(
              stats?.generation_ms
                ? ((stats.generated_tokens ?? NaN) * 1000) / stats.generation_ms
                : undefined,
            ),
          ],
        ];
  return (
    <>
      {modal.block_id && (
        <div class="dialog-actions" role="group" aria-label="Statistics scope">
          {(["turn", "session"] as const).map((value) => (
            <button
              aria-pressed={scope === value}
              onClick={() => setScope(value)}
            >
              {value === "turn" ? "Turn" : "Session"}
            </button>
          ))}
        </div>
      )}
      {!block && !snapshot ? (
        error ? (
          <LoadError error={error} retry={() => setAttempt(attempt + 1)} />
        ) : (
          <StatsSkeleton />
        )
      ) : (
        <>
          {!block && fork && (
            <p>Forked from “{fork.title}”. Totals count work in this fork.</p>
          )}
          <Rows rows={rows} />
          <UsageRows
            usage={
              summary
                ? summary.usage_reported === false
                  ? undefined
                  : summary.usage
                : block
                  ? block.usage_reported
                    ? block.usage
                    : undefined
                  : state?.usage
            }
          />
          <p class="muted small">
            {!block && !stats?.complete
              ? "Older activity was not fully recorded. "
              : ""}
            TTFT includes retries. Throughput uses total generation tokens
            divided by measured request time. Totals come from native counters
            and survive compaction. Missing provider data stays unreported.
          </p>
        </>
      )}
    </>
  );
}
