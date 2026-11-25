import { Button, SectionTitle } from "../../shared/ui.tsx";
import type { GitScope } from "./flow.ts";

export interface Change {
  path: string;
  state: string;
  staged?: string;
  working?: string;
}

export const scopeLabels = { working: "Working tree", staged: "Staged" };

const states: Record<string, string> = {
  A: "Added",
  C: "Copied",
  D: "Deleted",
  M: "Modified",
  R: "Renamed",
  T: "Type changed",
  U: "Unmerged",
};

export default function ChangeList({
  changes,
  file,
  scope,
  open,
}: {
  changes: Change[];
  file: string;
  scope?: GitScope;
  open: (path: string, scope?: GitScope) => void;
}) {
  return (
    <div class="files-changes dense">
      {(["working", "staged"] as const).map((group) => {
        const rows = changes.filter((c) => c[group]);
        if (!rows.length) return null;
        const label = scopeLabels[group];
        return (
          <section key={group} aria-label={label}>
            <SectionTitle>{label}</SectionTitle>
            <ul class="files-tree">
              {rows.map((change) => {
                const state = change[group]!;
                return (
                  <li key={change.path}>
                    <Button
                      variant="quiet"
                      class="entry tree-row"
                      aria-label={`${states[state] ?? state} ${change.path}`}
                      aria-current={
                        change.path === file && scope === group
                          ? "true"
                          : undefined
                      }
                      onClick={() => open(change.path, group)}
                    >
                      <span
                        class="files-state"
                        data-state={state}
                        title={states[state]}
                        aria-hidden="true"
                      >
                        {state}
                      </span>
                      <span class="files-name truncate">{change.path}</span>
                    </Button>
                  </li>
                );
              })}
            </ul>
          </section>
        );
      })}
    </div>
  );
}
