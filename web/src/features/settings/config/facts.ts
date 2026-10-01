import type { ConfigSetting, JSONValue } from "../../../shared/types.ts";

export const secret = (setting: ConfigSetting) =>
  setting.sensitivity !== "public";

export const shown = (value?: JSONValue) =>
  value == null || value === ""
    ? ""
    : typeof value === "boolean"
      ? value
        ? "On"
        : "Off"
      : String(value);

// The value a row shows: what applies, or whether a secret is set.
export const summary = (setting: ConfigSetting) =>
  secret(setting)
    ? setting.set || setting.source !== "default"
      ? "Set"
      : "Not set"
    : shown(setting.effective) || "Not set";

// Why that value applies, in one line.
export function reason(
  setting: ConfigSetting,
  find: (name: string) => ConfigSetting | undefined,
) {
  const builtin = shown(setting.default);
  const fallback = setting.follows
    ? `Follows ${find(setting.follows)?.label || setting.follows}`
    : builtin
      ? `Default: ${builtin}`
      : setting.fallback
        ? `Default: ${setting.fallback}`
        : "";
  if (setting.locked)
    return `Set ${setting.source === "cli" ? "on the command line" : "by the environment"}; change it there.`;
  if (setting.source === "project")
    return `This project sets ${shown(setting.effective)}.`;
  return setting.set === undefined ? fallback : fallback && `${fallback}.`;
}
