import type { State } from "../../shared/types.ts";
import { cost, count } from "../../shared/quantities.ts";
import { contextSummary } from "../../state/context.ts";
import { Button, DataText } from "../../shared/ui.tsx";

export function ContextSummary({
  state,
  open,
  online = true,
}: {
  state?: Pick<State, "context_tokens" | "context_window">;
  open?: () => void;
  online?: boolean;
}) {
  const title = `Estimated context: ${state?.context_tokens?.toLocaleString() ?? "—"}${state?.context_window ? ` / ${state.context_window.toLocaleString()}` : ""} tokens from serialized request bytes; provider billing usage is separate`;
  const summary = contextSummary(state?.context_tokens, state?.context_window);
  return open ? (
    <Button
      variant="quiet"
      aria-label="Raw context"
      title={`${title} · View raw context`}
      disabled={!online}
      onClick={open}
    >
      <DataText>{summary}</DataText>
    </Button>
  ) : (
    <span title={title} aria-label="Estimated context">
      <DataText>{summary}</DataText>
    </span>
  );
}

export function SessionSummary({
  state,
  open,
}: {
  state?: Pick<State, "statistics" | "turns" | "usage">;
  open: () => void;
}) {
  return (
    <Button variant="quiet" onClick={open} aria-label="Conversation statistics">
      <DataText>
        Conversation ·{" "}
        {count(state?.statistics?.recorded_turns ?? state?.turns)} turns
        {state?.usage?.cost_reported ? ` · ${cost(state.usage.cost)}` : ""}
      </DataText>
    </Button>
  );
}
