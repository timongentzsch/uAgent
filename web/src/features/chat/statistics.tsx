import { duration } from "../../shared/duration.ts";
import { count } from "../../shared/quantities.ts";
import type { StatisticsModal, Snapshot, State } from "../../shared/types.ts";
import { useEffect, useState } from "preact/hooks";
import { useResource } from "../../shared/use-resource.ts";
import { LoadError, Time } from "../../shared/ui.tsx";
import {
  Rows,
  StatisticsLayout,
  StatisticsLoading,
  UsageRows,
  type Row,
} from "../../shared/statistics-layout.tsx";
import { servedBy, servedByRows } from "../../shared/display.ts";

const rate = (value?: number) =>
  value && value > 0 ? `${count(value)} tok/s` : "Not recorded";
// A conversation the page holds is read as it stands there, live and with
// the older pages the person loaded; another one is fetched.
export default function Statistics({
  modal,
  held,
  loadSnapshot,
}: {
  modal: Extract<StatisticsModal, { type: "statistics" }>;
  held?: Snapshot;
  loadSnapshot: (id: string) => Promise<Snapshot>;
}) {
  const {
    value: fetched,
    error,
    retry,
    setValue,
  } = useResource(
    () => (held ? Promise.resolve(held) : loadSnapshot(modal.session_id)),
    [modal.session_id],
  );
  // Another conversation's numbers never stand in while these load.
  useEffect(() => setValue(undefined), [modal.session_id]);
  const snapshot = held || fetched;
  return snapshot ? (
    <StatisticsContent state={snapshot.state} nodeId={modal.node_id} />
  ) : error ? (
    <LoadError error={error} retry={retry} />
  ) : (
    <StatisticsLoading unit={modal.unit} />
  );
}

function StatisticsContent({
  state,
  nodeId,
}: {
  state?: State;
  nodeId?: string;
}) {
  const [scope, setScope] = useState<"turn" | "session">(
    nodeId ? "turn" : "session",
  );
  const target = nodeId
    ? state?.view?.nodes.find((node) => node.id === nodeId)
    : undefined;
  // A turn's footer carries its summary; any other node is one message.
  const unit = !nodeId
    ? undefined
    : target && target.kind !== "footer"
      ? "Message"
      : "Turn";
  const node = scope === "turn" ? target : undefined;
  const missingTurn = nodeId && scope === "turn" && !node;
  const stats = state?.statistics;
  const summary = node?.summary;
  const reply = node?.role === "assistant" ? node : undefined;
  const side = summary?.background_statistics;
  const fork = state?.view?.fork;
  const rows: Row[] = summary
    ? [
        ["Outcome", summary.outcome],
        ["Model at completion", summary.route || "Not recorded"],
        ["Model steps (parent)", count(summary.steps)],
        ["Model calls (all agents)", count(summary.model_calls)],
        ["Tool calls (all agents)", count(summary.tool_calls)],
        ["Side model calls", count(side?.model_calls)],
        ["Side tool calls", count(side?.tool_calls)],
        ["Duration", duration(summary.duration_ms)],
        ["TTFT", duration(summary.ttt_ms)],
        ["Throughput", rate(summary.tokens_per_second)],
      ]
    : node
      ? [
          [
            "Timestamp",
            node.time ? <Time value={node.time} /> : "Not recorded",
          ],
          [
            "Model",
            [reply?.route || "Not recorded", servedBy(reply?.served_by)]
              .filter(Boolean)
              .join(" · "),
          ],
          ["Status", node.state || "—"],
          ["Duration", duration(node.duration_ms)],
          ["TTFT", duration(reply?.ttft_ms)],
          ["Throughput", rate(reply?.tokens_per_second)],
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
          ...servedByRows(stats?.served_by),
        ];
  return (
    <StatisticsLayout unit={unit} scope={scope} change={setScope}>
      {missingTurn ? (
        <p role="status">
          This {unit?.toLowerCase()} is outside the loaded history. Load its
          retained messages and try again, or select Conversation.
        </p>
      ) : (
        <>
          {!node && fork && (
            <p>Forked from “{fork.title}”. Totals count work in this fork.</p>
          )}
          <Rows rows={rows} />
          <UsageRows
            usage={
              summary
                ? summary.usage_reported === false
                  ? undefined
                  : summary.usage
                : node
                  ? reply?.usage_reported
                    ? reply.usage
                    : undefined
                  : state?.usage
            }
          />
          <p class="muted small">
            {!node && !stats?.complete
              ? "Older activity was not fully recorded. "
              : ""}
            Throughput uses total generation tokens divided by measured request
            time. Totals come from native counters and survive compaction.
            All-agent counts include background and delegated work where
            labeled; parent and side durations stay separate because concurrent
            times cannot be added into wall time. Missing provider data stays
            unreported.
          </p>
        </>
      )}
    </StatisticsLayout>
  );
}
