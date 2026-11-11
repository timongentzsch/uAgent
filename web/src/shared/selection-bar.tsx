import type { ComponentChildren } from "preact";
import { Input } from "./form-controls.tsx";
import { Button } from "./ui.tsx";
import type { SelectionApi } from "./selection.ts";
import "./selection-bar.css";

export function RowCheck({
  picked,
  label,
  toggle,
}: {
  picked: boolean;
  label: string;
  toggle: () => void;
}) {
  return (
    <Input
      type="checkbox"
      class="row-check"
      checked={picked}
      aria-label={label}
      onChange={toggle}
    />
  );
}

export function SelectionBar({
  selection,
  children,
}: {
  selection: SelectionApi;
  children: ComponentChildren;
}) {
  return (
    <div class="selection-bar">
      <div role="toolbar" aria-label="Selection">
        <strong aria-live="polite">{`${selection.ids.size} selected`}</strong>
        <Button variant="secondary" onClick={() => selection.clear()}>
          {selection.mode ? "Done" : "Clear"}
        </Button>
      </div>
      {selection.mode && (
        <span class="selection-hint small">Click or tap rows to select</span>
      )}
      <div class="selection-actions">{children}</div>
    </div>
  );
}
