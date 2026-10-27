import { useState } from "preact/hooks";
import { duration } from "../../shared/duration.ts";
import { cost, count, plural, tokens } from "../../shared/quantities.ts";
import type { SessionRef, TurnSummary } from "../../shared/types.ts";
import { Button, Modal } from "../../shared/ui.tsx";
import TurnReview from "./turn-review.tsx";
import { formatStat } from "../../shared/display.ts";

// A turn's receipt and statistics.
export function TurnFooter({
  summary,
  session,
  open,
}: {
  summary: TurnSummary;
  session: SessionRef;
  open: () => void;
}) {
  const [reviewing, setReviewing] = useState(false);
  const files = summary.files || [];
  const outcome = summary.outcome !== "complete" &&
    summary.outcome !== "completed" && <span> · {summary.outcome}</span>;
  if (!files.length)
    return (
      <div class="turn-footer">
        {/* The name is the visible numbers, introduced for a listener. */}
        <Button variant="quiet" class="turn-summary" onClick={open}>
          <span class="sr-only">Turn statistics: </span>
          {summary.usage_reported !== false && (
            <span class="turn-tokens">
              {count(tokens(summary.usage))} tokens ·{" "}
            </span>
          )}
          <span>
            {count(summary.model_calls ?? summary.steps)} model calls ·{" "}
            {count(summary.tool_calls)} tools · {duration(summary.duration_ms)}
          </span>
          {outcome}
        </Button>
      </div>
    );
  // A turn that changed files leads with them: its receipt, and the way
  // back (the review sheet's Undo).
  const lines = files.reduce<[number, number]>(
    (sum, file) => [sum[0] + file.added, sum[1] + file.removed],
    [0, 0],
  );
  const spent = summary.usage.cost > 0 && summary.usage.cost_reported !== false;
  return (
    <div class="turn-footer">
      <Button
        variant="quiet"
        class="turn-summary"
        aria-haspopup="dialog"
        onClick={() => setReviewing(true)}
      >
        <span class="sr-only">Review changes: </span>
        {[plural(files.length, "file"), formatStat(lines)]
          .filter(Boolean)
          .join(" · ")}
      </Button>
      <Button variant="quiet" class="turn-summary" onClick={open}>
        <span class="sr-only">Turn statistics: </span>
        <span aria-hidden="true">· </span>
        {spent && `${cost(summary.usage.cost)} · `}
        {duration(summary.duration_ms)}
        {outcome}
      </Button>
      {reviewing && (
        <Modal
          title="Changed files"
          layout="sheet"
          size="narrow"
          close={() => setReviewing(false)}
        >
          <TurnReview files={files} turn={summary.turn} session={session} />
        </Modal>
      )}
    </div>
  );
}
