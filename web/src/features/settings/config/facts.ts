import type {
  ConfigScope,
  ConfigSetting,
  JSONValue,
} from "../../../shared/types.ts";

// A scope where a setting is saved from Settings: a conversation's own
// choices are made in the conversation.
export type SavedScope = Exclude<ConfigScope, "conversation">;

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

// A held value as the host takes it back.
export const raw = (value?: JSONValue) =>
  value == null ? "" : typeof value === "boolean" ? `${+value}` : String(value);

// Whether this project's value wins over the one saved at `scope`.
const overridden = (setting: ConfigSetting, scope: SavedScope) =>
  scope === "user" && setting.set?.project !== undefined;

// What applies when `scope` holds nothing: the value its "unset" option and
// its empty field name.
export function inherited(
  setting: ConfigSetting,
  scope: SavedScope,
  find: (name: string) => ConfigSetting | undefined,
) {
  const above = scope === "project" ? setting.set?.user : undefined;
  if (above !== undefined) return secret(setting) ? "set" : shown(above);
  return (
    shown(setting.default) ||
    (setting.follows
      ? `follows ${find(setting.follows)?.label || setting.follows}`
      : setting.fallback || "not set")
  );
}

// What a field says under its name, only when there is something to say:
// who decides instead, what it overrides, or that a restart is due.
export function note(
  setting: ConfigSetting,
  scope: SavedScope,
  find: (name: string) => ConfigSetting | undefined,
  restart = false,
) {
  if (setting.locked)
    return setting.source === "cli"
      ? "Set on the command line for this run"
      : `Set by environment variable ${setting.name} — change it there`;
  if (overridden(setting, scope)) return "Overridden in this project";
  if (scope === "project" && setting.set?.project !== undefined)
    return `Overrides ${inherited(setting, scope, find)} from all conversations`;
  return restart ? "Takes effect after restart" : undefined;
}
