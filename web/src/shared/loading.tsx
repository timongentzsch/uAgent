import { SearchSelect } from "./search-select.tsx";
import {
  Actions,
  Button,
  Field,
  Placeholder,
  Select,
  Row,
  Switch,
} from "./ui.tsx";

// Loading states that must exist before their feature's code: each draws the
// feature's own primitives from sample data (see <Placeholder>).

// The model form before its catalogue, also its code's fallback.
export function ModelLoading({
  close,
  defaults = true,
}: {
  close: () => void;
  defaults?: boolean;
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
        {defaults && (
          <ModelDefaults checked={false} disabled change={() => {}} />
        )}
      </Placeholder>
      <ModelActions close={close} />
    </div>
  );
}

// Shared by the loaded form and its placeholder, including wrapped detail text.
export function ModelDefaults({
  checked,
  disabled,
  change,
}: {
  checked: boolean;
  disabled?: boolean;
  change: (checked: boolean) => void;
}) {
  return (
    <Row
      label="Also use for new conversations"
      detail="Otherwise only this conversation changes."
    >
      <Switch
        label="Also use for new conversations"
        checked={checked}
        disabled={disabled}
        onChange={change}
      />
    </Row>
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
