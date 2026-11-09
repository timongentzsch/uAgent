import type { ComponentChildren } from "preact";
import { Input } from "./form-controls.tsx";
import { Button } from "./ui.tsx";
import type { SelectionApi } from "./selection.ts";
import "./selection-bar.css";

// What a list draws while it selects (see selection.ts): each row's
// checkbox, and the bar with the count, Clear (Done in select mode) and what
// can be done to all of them.

export function RowCheck({
  picked,
  label,
  toggle,
}: {
  picked: boolean;
  // "Select …", the row's name.
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
      <div class="selection-actions">{children}</div>
    </div>
  );
}
