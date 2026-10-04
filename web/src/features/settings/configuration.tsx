import { useState } from "preact/hooks";
import { plural } from "../../shared/quantities.ts";
import type { ConfigSetting, Session } from "../../shared/types.ts";
import {
  ConfirmModal,
  Group,
  Input,
  LoadError,
  Row,
} from "../../shared/ui.tsx";
import { SettingRowsLoading } from "./loading.tsx";
import { note, type SavedScope } from "./config/facts.ts";
import { RestartNotice } from "./config/restart-notice.tsx";
import { SettingItem } from "./config/setting-item.tsx";
import { SettingSheet } from "./config/setting-sheet.tsx";
import { useConfiguration } from "./config/use-configuration.ts";

type Sections = [string, (setting: ConfigSetting) => boolean][];

// What each list without a section says: all conversations' is Advanced
// (what you changed, what is locked, a search over the rest); a project's is
// its overrides and a search to add one.
const WHOLE = {
  user: {
    find: "Find a setting",
    held: "Changed",
    rest: "Everything else is at its default. Search to change one.",
    reset: "Reset all to defaults",
    none: "Everything is at its default",
    action: "Reset all",
  },
  project: {
    find: "Override a setting for this project",
    held: "Overrides",
    rest: "Everything else is the same as all conversations. Search to override one.",
    reset: "Remove all overrides",
    none: "This project overrides nothing",
    action: "Remove all",
  },
} as const;

// The settings saved at `scope` as rows. A section (`filter`) lists its
// own; without one the list is whole: what that scope holds, and a search
// over the rest.
export default function Configuration({
  session,
  online,
  scope = "user",
  filter,
  sections,
  sessions = [],
  display,
}: {
  session?: Session;
  online: boolean;
  scope?: SavedScope;
  filter?: (setting: ConfigSetting) => boolean;
  sections?: Sections;
  sessions?: Session[];
  // This browser's own display settings, reset with all conversations'.
  display?: { changed: number; reset: () => void };
}) {
  const config = useConfiguration(session, scope);
  const whole = WHOLE[scope];
  const [query, setQuery] = useState("");
  const [editing, setEditing] = useState("");
  const [confirm, setConfirm] = useState(false);
  const find = (name: string) =>
    config.settings.find((setting) => setting.name === name);
  const changed = config.settings.filter(
    (setting) =>
      setting.set?.[scope] !== undefined && setting.sensitivity === "public",
  );
  const needle = query.trim().toLowerCase();
  const groups: Sections = filter
    ? (sections ?? [["", () => true]])
    : needle
      ? [["Results", () => true]]
      : [
          [whole.held, (setting) => setting.set?.[scope] !== undefined],
          ["Locked", (setting) => scope === "user" && setting.locked],
        ];
  const listed = config.settings.filter(
    (setting) =>
      (filter ? filter(setting) : true) &&
      (!needle ||
        `${setting.label} ${setting.name} ${setting.purpose || ""} ${setting.description}`
          .toLowerCase()
          .includes(needle)),
  );
  // Each setting goes to the first group that takes it.
  const taken = new Set<string>();
  const rows = groups.map(([title, member]) => {
    const own = listed.filter(
      (setting) => !taken.has(setting.name) && member(setting),
    );
    own.forEach((setting) => taken.add(setting.name));
    return [title, own] as const;
  });
  const disabled = config.busy || !online;
  const edited = editing ? find(editing) : undefined;
  const resets = changed.length + (display?.changed || 0);
  if (!config.answered)
    return (
      <Group footer="The conversation's runtime answers for its project, and is busy with its turn.">
        <Row label="Available when the running turn ends" />
      </Group>
    );
  return (
    <section class="configuration">
      {!filter && (
        <div class="group-block">
          <Input
            type="search"
            aria-label={whole.find}
            placeholder={whole.find}
            value={query}
            onInput={(event) => setQuery(event.currentTarget.value)}
          />
        </div>
      )}
      {config.restart.length > 0 && (
        <RestartNotice
          key={config.restart.join()}
          keys={config.restart.map((key) => find(key)?.label || key)}
          host={config.restart.some((key) => find(key)?.category === "web")}
          running={sessions.filter((item) => item.generation).length}
        />
      )}
      {config.shadowed.length > 0 && (
        <p class="group-footer" role="status">
          {config.shadowed.map((key) => find(key)?.label || key).join(", ")}:
          saved, but a value set in the environment, on the command line, by
          this project or in this conversation keeps winning.
        </p>
      )}
      {config.error != null && !edited ? (
        <LoadError error={config.error} retry={config.retry} />
      ) : (
        !config.loaded && <SettingRowsLoading />
      )}
      {rows.map(
        ([title, settings]) =>
          settings.length > 0 && (
            <Group key={title} title={title || undefined}>
              {settings.map((setting) => (
                <SettingItem
                  key={setting.name}
                  setting={setting}
                  scope={scope}
                  note={note(
                    setting,
                    scope,
                    find,
                    config.restart.includes(setting.name),
                  )}
                  disabled={disabled}
                  open={() => setEditing(setting.name)}
                  toggle={(on) =>
                    void config.save(
                      on === setting.default
                        ? { key: setting.name, unset: true }
                        : { key: setting.name, value: on ? "1" : "0" },
                    )
                  }
                />
              ))}
            </Group>
          ),
      )}
      {!filter && config.loaded && (
        <>
          <p class="group-footer">
            {needle ? !listed.length && "No setting matches." : whole.rest}
          </p>
          <Group>
            <Row
              label={whole.reset}
              detail={resets ? plural(resets, "changed setting") : whole.none}
              destructive
              disabled={!resets || disabled}
              onClick={() => setConfirm(true)}
            />
          </Group>
        </>
      )}
      {edited && (
        <SettingSheet
          key={edited.name}
          setting={edited}
          scope={scope}
          find={find}
          busy={disabled}
          error={config.error}
          save={config.save}
          close={() => setEditing("")}
        />
      )}
      {confirm && (
        <ConfirmModal
          title={whole.reset}
          action={whole.action}
          busy={config.busy}
          error={config.error}
          close={() => setConfirm(false)}
          confirm={async () => {
            if (await config.reset()) {
              display?.reset();
              setConfirm(false);
            }
          }}
        >
          {scope === "user"
            ? "Every setting changed for all conversations and this browser's display settings return to their defaults."
            : "Every setting this project overrides is the same as all conversations again."}{" "}
          API keys and other secrets are kept.
        </ConfirmModal>
      )}
    </section>
  );
}
