import { Field, Placeholder, Select } from "./ui.tsx";

// Loading states that must exist before their feature's code: each draws the
// feature's own primitives from sample data (see <Placeholder>).

// The model form before its catalogue, also its code's fallback.
export function ModelLoading({ close }: { close: () => void }) {
  return (
    <div class="model-form">
      <Placeholder label="Loading models…">
        <Field label="Model">
          <Select aria-label="Model" />
        </Field>
        <div class="field-row">
          <Field label="Effort">
            <Select aria-label="Effort" />
          </Field>
          <Field label="Variant">
            <Select aria-label="Variant" />
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
    <div class="dialog-actions">
      <button type="button" onClick={close}>
        Cancel
      </button>
      <button
        type="button"
        class="primary"
        disabled={!apply || disabled || busy}
        onClick={apply}
      >
        {busy ? "Applying…" : "Apply"}
      </button>
    </div>
  );
}
