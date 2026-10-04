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
import { RestartNotice } from "./config/restart-notice.tsx";
import { SettingItem } from "./config/setting-item.tsx";
import { SettingSheet } from "./config/setting-sheet.tsx";
import { useConfiguration } from "./config/use-configuration.ts";

type Sections = [string, (setting: ConfigSetting) => boolean][];

// The host's settings as rows. A section (`filter`) lists its own; without
// one this is Advanced: what you changed, what is locked, and a search over
// the rest.
export default function Configuration({
  session,
  online,
  filter,
  sections,
  sessions = [],
  display,
}: {
  session?: Session;
  online: boolean;
  filter?: (setting: ConfigSetting) => boolean;
  sections?: Sections;
  sessions?: Session[];
  // This device's own display settings, reset with the host's.
  display?: { changed: number; reset: () => void };
}) {
  const config = useConfiguration(session);
  const [query, setQuery] = useState("");
  const [editing, setEditing] = useState("");
  const [confirm, setConfirm] = useState(false);
  const find = (name: string) =>
    config.settings.find((setting) => setting.name === name);
  const changed = config.settings.filter(
    (setting) =>
      setting.set?.user !== undefined && setting.sensitivity === "public",
  );
  const needle = query.trim().toLowerCase();
  const groups: Sections = filter
    ? (sections ?? [["", () => true]])
    : needle
      ? [["Results", () => true]]
      : [
          ["Changed", (setting) => setting.set?.user !== undefined],
          ["Locked", (setting) => setting.locked],
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
  return (
    <section class="configuration">
      {!filter && (
        <div class="group-block">
          <Input
            type="search"
            aria-label="Find a setting"
            placeholder="Find a setting"
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
          saved, but a value set in the environment, on the command line or by
          this project keeps winning.
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
            {needle
              ? !listed.length && "No setting matches."
              : "Everything else is at its default. Search to change one."}
          </p>
          <Group>
            <Row
              label="Reset all to defaults"
              detail={
                resets
                  ? plural(resets, "changed setting")
                  : "Everything is at its default"
              }
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
          find={find}
          busy={disabled}
          error={config.error}
          save={config.save}
          close={() => setEditing("")}
        />
      )}
      {confirm && (
        <ConfirmModal
          title="Reset all to defaults"
          action="Reset all"
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
          Every changed setting and this device's display settings return to
          their defaults. API keys and other secrets are kept.
        </ConfirmModal>
      )}
    </section>
  );
}
