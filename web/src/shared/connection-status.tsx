export type ConnectionPhase =
  "connected" | "connecting" | "reconnecting" | "disconnected";
export type LedState = "idle" | "active" | "running" | "failed";

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
      // Anything but connected stands out as a pill (style.css).
      class={`activity-status${phase === "connected" ? "" : " connection-pill"} ${className}`}
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
