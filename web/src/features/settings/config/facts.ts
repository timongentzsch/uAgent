import type {
  ConfigScope,
  ConfigSetting,
  JSONValue,
} from "../../../shared/types.ts";

// A scope where a setting is saved from Settings: a conversation's own
// choices are made in the conversation.
export type SavedScope = Exclude<ConfigScope, "conversation">;

// Every place a value can come from, as people read it.
export const SCOPE: Record<ConfigSetting["source"], string> = {
  default: "built in",
  user: "All conversations",
  project: "This project",
  conversation: "This conversation",
  environment: "the environment",
  cli: "the command line",
  file: "the config file",
};

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

// The narrower scope whose value wins over the one saved at `scope`.
const overriding = (setting: ConfigSetting, scope: SavedScope) =>
  setting.set?.conversation !== undefined
    ? "conversation"
    : scope === "user" && setting.set?.project !== undefined
      ? "project"
      : undefined;

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

// The value a row shows: what `scope` holds or passes on, or whether a
// secret is set.
export function summary(setting: ConfigSetting, scope: SavedScope = "user") {
  const own = setting.set?.[scope];
  if (secret(setting))
    return own || (scope === "user" && setting.source !== "default")
      ? "Set"
      : "Not set";
  if (scope === "project") return shown(own);
  return (
    (overriding(setting, scope)
      ? shown(own) || shown(setting.default)
      : shown(setting.effective)) || "Not set"
  );
}

// What a row says beside its name, only when there is something to say:
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
  const winner = overriding(setting, scope);
  if (winner) return `Overridden in this ${winner}`;
  if (scope === "project")
    return `Overrides ${inherited(setting, scope, find)} from all conversations`;
  return restart ? "Takes effect after restart" : undefined;
}

// Where the value in effect comes from, in one line.
export function origin(
  setting: ConfigSetting,
  find: (name: string) => ConfigSetting | undefined,
) {
  if (setting.locked) return `${note(setting, "user", find)}.`;
  if (setting.source === "default") {
    if (setting.follows)
      return `Follows ${find(setting.follows)?.label || setting.follows}.`;
    const builtin = shown(setting.default);
    return builtin
      ? `In effect: ${builtin}, built in.`
      : setting.fallback
        ? `Default: ${setting.fallback}.`
        : "";
  }
  const value = secret(setting) ? "a value" : shown(setting.effective);
  return `In effect: ${value}, from ${SCOPE[setting.source]}.`;
}
