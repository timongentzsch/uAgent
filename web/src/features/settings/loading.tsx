import { Group, Placeholder, Row } from "../../shared/ui.tsx";
import "./settings.css";

// Registry settings before they load, drawn from sample data (see
// <Placeholder>).
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
