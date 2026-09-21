import { duration } from "../../shared/duration.ts";
import { count } from "../../shared/quantities.ts";
import type { TurnSummary } from "../../shared/types.ts";

// Turn footer button. Lives apart from statistics.tsx (the lazily loaded
// statistics dialog): every transcript message row renders this, so a
// static import of the dialog chunk would chain the whole transcript
// behind it and freeze message rendering until the dialog arrives.
export function TurnFooter({
  summary,
  open,
}: {
  summary: TurnSummary;
  open: () => void;
}) {
  return (
    <button
      class="quiet turn-summary"
      onClick={open}
      aria-label="Turn statistics"
    >
      {summary.usage_reported !== false && (
        <span class="turn-tokens">
          {count((summary.usage.input || 0) + (summary.usage.output || 0))}{" "}
          tokens ·{" "}
        </span>
      )}
      <span>
        {count(summary.model_calls ?? summary.steps)} model calls ·{" "}
        {count(summary.tool_calls)} tools · {duration(summary.duration_ms)}
      </span>
      {summary.outcome !== "complete" && summary.outcome !== "completed" && (
        <span> · {summary.outcome}</span>
      )}
    </button>
  );
}
