import type { ComponentChildren } from "preact";
import type { StatisticsUnit } from "./types.ts";
import { Spinner } from "./ui.tsx";

// Code loading, data loading and loaded statistics share the same toolbar;
// a unit (the turn or message opened) adds its scope beside Session.
export function StatisticsLayout({
  unit,
  scope,
  change,
  children,
}: {
  unit?: StatisticsUnit;
  scope: "turn" | "session";
  change?: (scope: "turn" | "session") => void;
  children: ComponentChildren;
}) {
  return (
    <>
      {unit && (
        <div class="dialog-actions" role="group" aria-label="Statistics scope">
          {(["turn", "session"] as const).map((value) => (
            <button
              type="button"
              key={value}
              disabled={!change}
              aria-pressed={scope === value}
              onClick={() => change?.(value)}
            >
              {value === "turn" ? unit : "Session"}
            </button>
          ))}
        </div>
      )}
      {children}
    </>
  );
}

export function StatisticsLoading({ unit }: { unit?: StatisticsUnit }) {
  return (
    <StatisticsLayout unit={unit} scope={unit ? "turn" : "session"}>
      <Spinner label="Loading statistics…" surface />
    </StatisticsLayout>
  );
}
