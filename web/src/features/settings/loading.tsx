import { Group, Placeholder, Row } from "../../shared/ui.tsx";
import "./settings.css";

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
          <span class="setting-summary truncate">Not set</span>
        </Row>
      ))}
    </Group>
  );
}
