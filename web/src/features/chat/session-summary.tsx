import type { State } from "../../shared/types.ts";
import { cost, count } from "../../shared/quantities.ts";
import { contextSummary } from "../../state/context.ts";

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
    <button
      type="button"
      class="quiet"
      aria-label="Raw context"
      title={`${title} · View raw context`}
      disabled={!online}
      onClick={open}
    >
      {summary}
    </button>
  ) : (
    <span title={title} aria-label="Estimated context">
      {summary}
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
    <button
      type="button"
      class="quiet"
      onClick={open}
      aria-label="Session statistics"
    >
      Session · {count(state?.statistics?.recorded_turns ?? state?.turns)} turns
      {state?.usage?.cost_reported ? ` · ${cost(state.usage.cost)}` : ""}
    </button>
  );
}
