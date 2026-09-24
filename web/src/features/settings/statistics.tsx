import { duration } from "../../shared/duration.ts";
import { cost, count } from "../../shared/quantities.ts";
import type {
  Usage,
  StatisticsModal,
  Snapshot,
  State,
} from "../../shared/types.ts";
import { useEffect, useState } from "preact/hooks";
import { LoadError } from "../../shared/ui.tsx";
import {
  StatisticsLayout,
  StatisticsLoading,
} from "../../shared/statistics-layout.tsx";
import { presentMessages } from "../chat/message-view.ts";

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
        ["Cost", usage?.cost_reported ? cost(usage.cost) : "Not reported"],
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
  return snapshot ? (
    <StatisticsContent state={snapshot.state} blockId={modal.block_id} />
  ) : error ? (
    <LoadError error={error} retry={() => setAttempt(attempt + 1)} />
  ) : (
    <StatisticsLoading turn={!!modal.block_id} />
  );
}

export function StatisticsContent({
  state,
  blockId,
}: {
  state?: State;
  blockId?: string;
}) {
  const [scope, setScope] = useState<"turn" | "session">(
    blockId ? "turn" : "session",
  );
  const presented = state?.view ? presentMessages(state.view.blocks) : [];
  const flattened = presented.flatMap((row) =>
    row.children ? [row, ...row.children] : [row],
  );
  const block =
    scope === "turn"
      ? flattened.find(
          (row) =>
            row.key === blockId ||
            row.id === blockId ||
            row.response_id === blockId ||
            row.occurrence_id === blockId,
        )
      : undefined;
  const missingTurn = blockId && scope === "turn" && !block;
  const stats = state?.statistics;
  const summary = block?.summary;
  const side = summary?.background_statistics;
  const fork = state?.view?.fork;
  const toolScope =
    summary?.direct_tool_calls === undefined ? "recorded parent" : "all agents";
  const modelScope =
    summary?.direct_model_calls === undefined
      ? "recorded parent"
      : "all agents";
  const rows: [string, string][] = summary
    ? [
        ["Outcome", summary.outcome],
        ["Model at completion", summary.route || "Not recorded"],
        ["Model steps (parent)", count(summary.steps)],
        [`Model calls (${modelScope})`, count(summary.model_calls)],
        [`Tool calls (${toolScope})`, count(summary.tool_calls)],
        ["Side model calls", count(side?.model_calls)],
        ["Side tool calls", count(side?.tool_calls)],
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
            fork ? "Turns in fork (all agents)" : "Turns (all agents)",
            count(
              stats?.recorded_turns === undefined
                ? state?.turns === undefined
                  ? undefined
                  : state.turns - (fork?.turns || 0)
                : stats.recorded_turns + (stats.side_recorded_turns || 0),
            ),
          ],
          [
            "Tool calls (all agents)",
            count(
              stats?.tool_calls === undefined
                ? stats?.complete
                  ? 0
                  : undefined
                : stats.tool_calls + (stats.side_tool_calls || 0),
            ),
          ],
          [
            "Model calls (all agents)",
            count(
              stats?.model_calls === undefined
                ? stats?.complete
                  ? 0
                  : undefined
                : stats.model_calls + (stats.side_model_calls || 0),
            ),
          ],
          ["Parent turn time", duration(stats?.duration_ms)],
          ["Side agent time (summed)", duration(stats?.side_duration_ms)],
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
    <StatisticsLayout turn={!!blockId} scope={scope} change={setScope}>
      {missingTurn ? (
        <p role="status">
          This turn is outside the loaded history. Load its retained messages
          and try again, or select Session.
        </p>
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
            and survive compaction. All-agent counts include background and
            delegated work where labeled; parent and side durations stay
            separate because concurrent times cannot be added into wall time.
            Missing provider data stays unreported.
          </p>
        </>
      )}
    </StatisticsLayout>
  );
}
