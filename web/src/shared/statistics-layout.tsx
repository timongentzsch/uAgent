import type { ComponentChildren } from "preact";
import { Spinner } from "./ui.tsx";

// Code loading, data loading and loaded statistics share the same toolbar.
export function StatisticsLayout({
  turn,
  scope,
  change,
  children,
}: {
  turn: boolean;
  scope: "turn" | "session";
  change?: (scope: "turn" | "session") => void;
  children: ComponentChildren;
}) {
  return (
    <>
      {turn && (
        <div class="dialog-actions" role="group" aria-label="Statistics scope">
          {(["turn", "session"] as const).map((value) => (
            <button
              type="button"
              key={value}
              disabled={!change}
              aria-pressed={scope === value}
              onClick={() => change?.(value)}
            >
              {value === "turn" ? "Turn" : "Session"}
            </button>
          ))}
        </div>
      )}
      {children}
    </>
  );
}

export function StatisticsLoading({ turn = false }: { turn?: boolean }) {
  return (
    <StatisticsLayout turn={turn} scope={turn ? "turn" : "session"}>
      <Spinner label="Loading statistics…" surface />
    </StatisticsLayout>
  );
}
