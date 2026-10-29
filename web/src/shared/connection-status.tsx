import type { ConnectionPhase } from "./types.ts";
// The status LED and the connection line in the sidebar.

type LedState = "idle" | "active" | "running" | "failed";

export function StatusLed({ state }: { state: LedState }) {
  return <span class={`status-led ${state}`} aria-hidden="true" />;
}

export function ConnectionStatus({
  phase,
  className = "",
}: {
  phase: ConnectionPhase;
  className?: string;
}) {
  const busy = phase === "connecting" || phase === "reconnecting";
  const label = {
    connected: "Connected",
    connecting: "Connecting…",
    reconnecting: "Reconnecting…",
    disconnected: "Disconnected",
  }[phase];
  return (
    <span
      // Anything but connected stands out as a chip.
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
      {label}
    </span>
  );
}
