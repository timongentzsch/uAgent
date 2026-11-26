import { useState } from "preact/hooks";
import { plural } from "../../shared/quantities.ts";
import type { ConfigSetting, Session } from "../../shared/types.ts";
import {
  ConfirmModal,
  Group,
  LoadError,
  Notice,
  Row,
} from "../../shared/ui.tsx";
import {
  hostRestarts,
  itemized,
  note,
  type SavedScope,
} from "./config/facts.ts";
import { RestartNotice } from "./config/restart-notice.tsx";
import { SettingField } from "./config/setting-field.tsx";
import type { Config } from "./config/use-configuration.ts";
import "./configuration.css";

const WHOLE = {
  user: {
    reset: "Reset all to defaults",
    none: "Everything is at its default",
    action: "Reset all",
    confirm:
      "Every setting changed for all conversations returns to its default.",
  },
  project: {
    reset: "Remove all overrides",
    none: "This folder overrides nothing",
    action: "Remove all",
    confirm:
      "Every setting this folder overrides is the same as all conversations again.",
  },
} as const;

// What the host could not read or take, and what waits for a restart.
export function ConfigNotices({
  config,
  scope,
  folder,
  sessions,
}: {
  config: Config;
  scope: SavedScope;
  folder?: string;
  sessions: Session[];
}) {
  return (
    <>
      {config.error != null && (
        <LoadError error={config.error} retry={config.retry} />
      )}
      {config.problem && (
        <p class="group-footer">
          <Notice tone="error">{config.problem}</Notice>
        </p>
      )}
      {config.restart.length > 0 && (
        <RestartNotice
          key={config.restart.join()}
          keys={config.restart.map((key) => config.find(key)?.label || key)}
          host={config.restart.some((key) => hostRestarts(config.find(key)))}
          running={sessions.filter((item) => item.generation).length}
          folder={scope === "project" ? folder : undefined}
        />
      )}
    </>
  );
}

// One group of host settings, drawn from the registry alone.
export function SettingsGroup({
  config,
  label,
  settings,
  scope,
  folder,
  online,
}: {
  config: Config;
  label: string;
  settings: ConfigSetting[];
  scope: SavedScope;
  folder?: string;
  online: boolean;
}) {
  const field = (setting: ConfigSetting) => (
    <SettingField
      key={setting.name}
      reload={config.retry}
      setting={setting}
      scope={scope}
      folder={folder}
      find={config.find}
      note={note(
        setting,
        scope,
        config.find,
        config.restart.includes(setting.name),
      )}
      error={
        config.failed?.key === setting.name
          ? String(
              (config.failed.error as Error)?.message || config.failed.error,
            )
          : undefined
      }
      disabled={!online}
      online={online}
      save={config.save}
    />
  );
  // A list follows its group as a group of its own.
  const rows = settings.filter((setting) => !itemized(setting));
  return (
    <>
      {rows.length > 0 && <Group title={label}>{rows.map(field)}</Group>}
      {settings.filter(itemized).map(field)}
    </>
  );
}

// The settings file as it stands, and taking a whole scope back.
export function SettingsFile({
  config,
  scope,
  online,
}: {
  config: Config;
  scope: SavedScope;
  online: boolean;
}) {
  const whole = WHOLE[scope];
  const [confirm, setConfirm] = useState(false);
  const [asFile, setAsFile] = useState(false);
  const held = config.settings.filter(
    (setting) =>
      setting.set?.[scope] !== undefined && setting.sensitivity === "public",
  ).length;
  return (
    <>
      <Group
        title="Settings file"
        footer={
          config.file && (
            <>
              Saved in <code>{config.file}</code>, which an editor can open too:{" "}
              <code>settings.schema.json</code> beside it names and checks every
              setting, and a change is taken up at the next message.
            </>
          )
        }
      >
        <Row
          label={asFile ? "Hide the file" : "Show the file"}
          detail="As it stands, with keys and other secrets hidden."
          disabled={!config.loaded}
          expanded={asFile}
          onClick={() => setAsFile(!asFile)}
        />
        {asFile && (
          <pre class="configuration-file">
            <code>{JSON.stringify(config.document ?? {}, null, 2)}</code>
          </pre>
        )}
        <Row
          label={whole.reset}
          detail={held ? plural(held, "changed setting") : whole.none}
          destructive
          disabled={!config.loaded || !held || !online}
          onClick={() => setConfirm(true)}
        />
      </Group>
      {confirm && (
        <ConfirmModal
          title={whole.reset}
          action={whole.action}
          busy={config.busy}
          error={config.failed?.error}
          close={() => setConfirm(false)}
          confirm={async () => {
            if (await config.reset()) setConfirm(false);
          }}
        >
          {whole.confirm} API keys and other secrets are kept.
        </ConfirmModal>
      )}
    </>
  );
}

// A topic's rarely changed settings, folded until asked for.
export function ShowAll({
  count,
  shown,
  toggle,
}: {
  count: number;
  shown: boolean;
  toggle: () => void;
}) {
  return (
    <Group>
      <Row
        label={shown ? "Show fewer" : "Show all"}
        detail={shown ? undefined : plural(count, "rarely changed setting")}
        expanded={shown}
        onClick={toggle}
      />
    </Group>
  );
}
