import { useContext, useState } from "preact/hooks";
import { duration } from "../../shared/duration.ts";
import { cost, count, plural, tokens } from "../../shared/quantities.ts";
import type { Node, SessionRef } from "../../shared/types.ts";
import { Button, Modal, Time } from "../../shared/ui.tsx";
import TurnReview from "./turn-review.tsx";
import { formatStat } from "../../shared/display.ts";
import { MessageActions } from "./message-actions.ts";
import { MessageMenu } from "./message-menu.tsx";

// A turn's last line: the files it changed (which open their review and its
// Undo), what it cost, and at its end the answer's time and menu, which
// holds the turn's statistics.
export function TurnFooter({
  node,
  answer,
  session,
}: {
  node: Node;
  answer?: Node;
  session: SessionRef;
}) {
  const { statistics, http } = useContext(MessageActions);
  const [reviewing, setReviewing] = useState(false);
  const summary = node.summary;
  if (!summary) return null;
  const files = summary.files || [];
  const lines = files.reduce<[number, number]>(
    (sum, file) => [sum[0] + file.added, sum[1] + file.removed],
    [0, 0],
  );
  const spent = summary.usage.cost > 0 && summary.usage.cost_reported !== false;
  return (
    <div
      class="turn-footer"
      data-message-id={node.id}
      data-sequence={node.sequence}
    >
      {files.length > 0 && (
        <Button
          variant="quiet"
          class="turn-files-button"
          aria-haspopup="dialog"
          onClick={() => setReviewing(true)}
        >
          <span class="sr-only">Review changes: </span>
          {[plural(files.length, "file"), formatStat(lines)]
            .filter(Boolean)
            .join(" · ")}
        </Button>
      )}
      <span class="turn-summary">
        {summary.usage_reported !== false && (
          <span>{count(tokens(summary.usage))} tokens · </span>
        )}
        <span class="turn-calls">
          {plural(summary.model_calls ?? summary.steps, "model call")} ·{" "}
        </span>
        {spent && `${cost(summary.usage.cost)} · `}
        {duration(summary.duration_ms)}
        {summary.outcome !== "complete" && ` · ${summary.outcome}`}
      </span>
      <span class="message-meta">
        <Time value={answer?.time ?? node.time} />
        <MessageMenu
          label="Actions for this answer"
          node={node}
          reply={answer}
          statistics={statistics}
          http={http}
        />
      </span>
      {reviewing && (
        <Modal
          title="Changed files"
          layout="sheet"
          size="s"
          close={() => setReviewing(false)}
        >
          <TurnReview files={files} turn={summary.turn} session={session} />
        </Modal>
      )}
    </div>
  );
}
