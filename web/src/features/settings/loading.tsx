import { DialogHeader, Group, Placeholder, Row } from "../../shared/ui.tsx";
import "./settings.css";
import { SettingsNav } from "./settings-nav.tsx";
import { useMedia } from "../../shared/layout.ts";

// Settings' loading states, which exist before its code: each draws the
// feature's own primitives from sample data (see <Placeholder>).
const noop = () => {};

// Settings before its code arrives: the same header, scope list and page.
export function SettingsLoading() {
  const phone = useMedia("(max-width: 600px)");
  return (
    <>
      <DialogHeader title="Settings" />
      <div class="dialog-body settings-content">
        <SettingsNav current={phone ? undefined : "user"} select={noop} />
        <div class="settings-pane">
          {!phone && <h3 class="settings-pane-title">All conversations</h3>}
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
      {["Conversation model", "Reasoning effort", "API address"].map((name) => (
        <Row key={name} label={name}>
          <span class="setting-summary">Not set</span>
        </Row>
      ))}
    </Group>
  );
}
