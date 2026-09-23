import type { State } from "../../shared/types.ts";
import { count } from "../../shared/quantities.ts";

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
      {state?.usage?.cost_reported ? ` · $${state.usage.cost.toFixed(4)}` : ""}
    </button>
  );
}
