import { SearchSelect } from "./search-select.tsx";
import type { ComponentChildren } from "preact";
import { Actions, Button, Field, Placeholder, Select } from "./ui.tsx";

// Loading states that must exist before their feature's code loads.

export function ModelLoading({
  close,
  source,
}: {
  close: () => void;
  // Where a conversation's model comes from, known before the models are.
  source?: ComponentChildren;
}) {
  return (
    <div class="model-form">
      <Placeholder label="Loading models…">
        <SearchSelect
          label="Model"
          value=""
          options={[]}
          disabled
          change={() => {}}
        />
        <div class="field-row">
          <Field label="Effort">
            <Select aria-label="Effort" />
          </Field>
        </div>
      </Placeholder>
      {source}
      <ModelActions close={close} />
    </div>
  );
}

export function ModelActions({
  close,
  apply,
  disabled = false,
  busy = false,
}: {
  close: () => void;
  apply?: () => void;
  disabled?: boolean;
  busy?: boolean;
}) {
  return (
    <Actions>
      <Button onClick={close}>Cancel</Button>
      <Button
        variant="primary"
        busy={busy}
        disabled={!apply || disabled}
        onClick={apply}
      >
        Apply
      </Button>
    </Actions>
  );
}
