import type { ComponentChildren } from "preact";
import type { StatisticsUnit, Usage } from "./types.ts";
import { Button, DataText, Placeholder } from "./ui.tsx";
import { cost, count } from "./quantities.ts";

// Labelled values, the one shape every statistics scope lists.
export type Row = [string, ComponentChildren];
export function Rows({ rows }: { rows: Row[] }) {
  return (
    <dl class="stats">
      {rows.map(([label, value]) => (
        <div key={label}>
          <dt>
            <DataText>{label}</DataText>
          </dt>
          <dd>
            <DataText>{value}</DataText>
          </dd>
        </div>
      ))}
    </dl>
  );
}
export function UsageRows({ usage }: { usage?: Usage }) {
  return (
    <Rows
      rows={[
        ["Input tokens (uncached)", count(usage?.input)],
        ["Output tokens", count(usage?.output)],
        ["Reasoning tokens", count(usage?.reasoning)],
        ["Cache read tokens", count(usage?.cache_read)],
        ["Cache write tokens", count(usage?.cache_write)],
        ["Cost", usage?.cost_reported ? cost(usage.cost) : "Not reported"],
      ]}
    />
  );
}

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
              {value === "turn" ? unit : "Conversation"}
            </Button>
          ))}
        </div>
      )}
      {children}
    </>
  );
}

// How many rows each scope lists before its usage rows.
const SCOPE_ROWS = { Turn: 10, Message: 7, Conversation: 9 };
export function StatisticsLoading({ unit }: { unit?: StatisticsUnit }) {
  return (
    <StatisticsLayout unit={unit} scope={unit ? "turn" : "session"}>
      <Placeholder label="Loading statistics…">
        <Rows
          rows={Array.from(
            { length: SCOPE_ROWS[unit || "Conversation"] },
            (_, index): Row => [`Recorded value ${index}`, "Not recorded"],
          )}
        />
        <UsageRows />
      </Placeholder>
    </StatisticsLayout>
  );
}
