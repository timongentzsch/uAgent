import type { ConfigSetting } from "../../../shared/types.ts";
import { Row, Switch } from "../../../shared/ui.tsx";
import { summary, type SavedScope } from "./facts.ts";

// A setting in a list: its name and what its scope holds. A switch flips in
// place where unsetting is flipping back; anything else opens its sheet.
export function SettingItem({
  setting,
  scope,
  note,
  disabled,
  open,
  toggle,
}: {
  setting: ConfigSetting;
  scope: SavedScope;
  // Who decides instead, what it overrides, or that a restart is due.
  note?: string;
  disabled: boolean;
  open: () => void;
  toggle: (on: boolean) => void;
}) {
  if (setting.type === "boolean" && !setting.locked && scope === "user")
    return (
      <Row label={setting.label} detail={note || setting.purpose}>
        <Switch
          label={setting.label}
          checked={summary(setting) === "On"}
          disabled={disabled}
          onChange={toggle}
        />
      </Row>
    );
  return (
    <Row label={setting.label} detail={note} onClick={open}>
      <span
        class="setting-summary"
        data-default={setting.source === "default" ? "" : undefined}
      >
        {summary(setting, scope)}
      </span>
    </Row>
  );
}
