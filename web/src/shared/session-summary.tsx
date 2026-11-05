import type { State } from "./types.ts";
import { cost, plural } from "./quantities.ts";
import { contextLeft } from "../state/context-summary.ts";
import { Button, DataText, updateChecked } from "./ui.tsx";

// What the composer's one field says of a conversation: context left, turns,
// cost and guidance waiting. It opens the statistics.
function SessionSummaryView({
  state,
  queued,
  open,
}: {
  state?: Pick<
    State,
    "context_tokens" | "context_window" | "statistics" | "turns" | "usage"
  >;
  // Guidance the host holds that this device lists nowhere else.
  queued?: number;
  open: () => void;
}) {
  const { context_tokens: used, context_window: limit } = state ?? {};
  const turns = state?.statistics?.recorded_turns ?? state?.turns;
  const title = `Estimated context: ${used?.toLocaleString() ?? "—"}${limit ? ` / ${limit.toLocaleString()}` : ""} tokens from serialized request bytes; provider billing usage is separate`;
  const parts = [
    contextLeft(used, limit),
    turns === undefined ? "" : plural(turns, "turn"),
    state?.usage?.cost_reported ? cost(state.usage.cost) : "",
    queued ? `${queued} queued` : "",
  ];
  return (
    <Button
      variant="quiet"
      onClick={open}
      aria-label="Conversation statistics"
      title={title}
    >
      <span class="truncate">
        <DataText>{parts.filter(Boolean).join(" · ")}</DataText>
      </span>
    </Button>
  );
}

// The composer renders with every key pressed; this only when what it
// counts changes.
export const SessionSummary = updateChecked(SessionSummaryView, [
  "context_tokens",
  "context_window",
  "statistics",
  "turns",
  "usage",
]);
