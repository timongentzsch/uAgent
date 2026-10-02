import type { ConfigSetting } from "../../../shared/types.ts";
import { Row, Switch } from "../../../shared/ui.tsx";
import { summary } from "./facts.ts";

// A setting in a list: its name and what applies. A switch flips in place;
// anything else opens its sheet.
export function SettingItem({
  setting,
  disabled,
  open,
  toggle,
}: {
  setting: ConfigSetting;
  disabled: boolean;
  open: () => void;
  toggle: (on: boolean) => void;
}) {
  if (setting.type === "boolean" && !setting.locked)
    return (
      <Row label={setting.label} detail={setting.purpose}>
        <Switch
          label={setting.label}
          checked={summary(setting) === "On"}
          disabled={disabled}
          onChange={toggle}
        />
      </Row>
    );
  return (
    <Row
      label={setting.label}
      detail={setting.locked ? "Locked" : undefined}
      onClick={open}
    >
      <span
        class="setting-summary"
        data-default={setting.source === "default" ? "" : undefined}
      >
        {summary(setting)}
      </span>
    </Row>
  );
}
