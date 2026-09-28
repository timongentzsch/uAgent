import {
  DialogHeader,
  Field,
  Group,
  Input,
  Placeholder,
  Select,
  SettingRow,
} from "./ui.tsx";
import "../features/settings/settings.css";
import { SettingsNav } from "../features/settings/settings-nav.tsx";
import { useMedia } from "./layout.ts";

// Loading states that must exist before their feature's code: each draws the
// feature's own primitives from sample data (see <Placeholder>).
const noop = () => {};

// Settings before its code arrives: the same header, section list and pane.
export function SettingsLoading() {
  const phone = useMedia("(max-width: 600px)");
  return (
    <>
      <DialogHeader title="Settings" />
      <div class="dialog-body settings-content">
        <SettingsNav current={phone ? undefined : "general"} select={noop} />
        <div class="settings-pane">
          {!phone && <h3 class="settings-pane-title">General</h3>}
          <Placeholder label="Loading settings…">
            <SettingRows />
          </Placeholder>
        </div>
      </div>
    </>
  );
}

// Registry settings before they load.
export function SettingRowsLoading() {
  return (
    <Placeholder label="Loading configuration…">
      <SettingRows />
    </Placeholder>
  );
}

function SettingRows() {
  return (
    <Group>
      {["UAGENT_MODEL", "UAGENT_REASONING_EFFORT", "UAGENT_BASE_URL"].map(
        (name) => (
          <SettingRow
            key={name}
            name={name}
            detail="What this setting changes and where it applies"
            reset={noop}
          >
            <Input aria-label={name} />
          </SettingRow>
        ),
      )}
    </Group>
  );
}

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
