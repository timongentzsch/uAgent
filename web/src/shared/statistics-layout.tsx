import type { ComponentChildren } from "preact";
import type { StatisticsUnit } from "./types.ts";
import { Button } from "./ui.tsx";

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
            <Button
              key={value}
              disabled={!change}
              aria-pressed={scope === value}
              onClick={() => change?.(value)}
            >
              {value === "turn" ? unit : "Session"}
            </Button>
          ))}
        </div>
      )}
      {children}
    </>
  );
}

// The rows each scope lists (its own, then usage), with placeholder text.
const ROWS = { Turn: 16, Message: 13, Session: 15 };
export function StatisticsLoading({ unit }: { unit?: StatisticsUnit }) {
  return (
    <StatisticsLayout unit={unit} scope={unit ? "turn" : "session"}>
      <span class="sr-only" role="status" aria-busy="true">
        Loading statistics…
      </span>
      <dl class="stats" aria-hidden="true">
        {Array.from({ length: ROWS[unit || "Session"] }, (_, index) => (
          <div key={index}>
            <dt>
              <span class="text-skeleton">
                {index % 3 ? "Model calls" : "Input tokens (all)"}
              </span>
            </dt>
            <dd>
              <span class="text-skeleton">000</span>
            </dd>
          </div>
        ))}
      </dl>
    </StatisticsLayout>
  );
}
