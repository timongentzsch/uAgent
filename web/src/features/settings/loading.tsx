import {
  DialogHeader,
  Group,
  Input,
  Placeholder,
  SettingRow,
} from "../../shared/ui.tsx";
import "./settings.css";
import { SettingsNav } from "./settings-nav.tsx";
import { useMedia } from "../../shared/layout.ts";

// Settings' loading states, which exist before its code: each draws the
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
