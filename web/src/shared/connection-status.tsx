import type { ConnectionPhase } from "./types.ts";

type LedState = "idle" | "active" | "running" | "failed";

export function StatusLed({ state }: { state: LedState }) {
  return <span class={`status-led ${state}`} aria-hidden="true" />;
}

export const connectionLabels = {
  connected: "Connected",
  connecting: "Connecting…",
  reconnecting: "Reconnecting…",
  disconnected: "Disconnected",
};

export function ConnectionStatus({
  phase,
  className = "",
}: {
  phase: ConnectionPhase;
  className?: string;
}) {
  const busy = phase === "connecting" || phase === "reconnecting";
  return (
    <span
      class={`turn-status${phase === "connected" ? "" : " chip chip-outline"} ${className}`}
      role="status"
      aria-live="polite"
      aria-busy={busy || undefined}
    >
      {busy ? (
        <span class="spinner" aria-hidden="true" />
      ) : (
        <StatusLed state={phase === "connected" ? "active" : "idle"} />
      )}
      {connectionLabels[phase]}
    </span>
  );
}
