import { SearchSelect } from "./search-select.tsx";
import { Actions, Button, Field, Placeholder, Select } from "./ui.tsx";

// Loading states that must exist before their feature's code loads.

export function ModelLoading({ close }: { close: () => void }) {
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
