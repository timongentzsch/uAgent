import { useState } from "preact/hooks";
import { plural } from "../../shared/quantities.ts";
import type { Session } from "../../shared/types.ts";
import {
  Button,
  ConfirmModal,
  Group,
  Input,
  LoadError,
  Row,
} from "../../shared/ui.tsx";
import { SettingRowsLoading } from "./loading.tsx";
import { note, type SavedScope } from "./config/facts.ts";
import { RestartNotice } from "./config/restart-notice.tsx";
import { SettingField } from "./config/setting-field.tsx";
import { useConfiguration } from "./config/use-configuration.ts";

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
    none: "This project overrides nothing",
    action: "Remove all",
    confirm:
      "Every setting this project overrides is the same as all conversations again.",
  },
} as const;

// How the connection settings fit together, said once above them.
const INTRO: Partial<Record<SavedScope, string>> = {
  user: "To connect, one of these is enough: an OpenRouter key, or an API address with its key. Named providers add further endpoints, each with its own address and key; a model is then chosen as provider/model.",
};

// Everything that can be saved at `scope`, under its groups, each setting
// edited in place. The box narrows the same rows.
export default function Configuration({
  scope,
  folder,
  version,
  online,
  sessions,
}: {
  scope: SavedScope;
  // The open conversation's folder: the project, and whose overrides show.
  folder?: string;
  // Counts every saved change, whoever made it.
  version: number;
  online: boolean;
  sessions: Session[];
}) {
  const config = useConfiguration(scope, folder, version);
  const whole = WHOLE[scope];
  const [query, setQuery] = useState("");
  const [confirm, setConfirm] = useState(false);
  // The same settings as the file holds them, for reading.
  const [asFile, setAsFile] = useState(false);
  const find = (name: string) =>
    config.settings.find((setting) => setting.name === name);
  const needle = query.trim().toLowerCase();
  const listed = config.settings.filter(
    (setting) =>
      !needle ||
      `${setting.label} ${setting.key} ${setting.name} ${setting.purpose || ""} ${setting.description}`
        .toLowerCase()
        .includes(needle),
  );
  const held = config.settings.filter(
    (setting) =>
      setting.set?.[scope] !== undefined && setting.sensitivity === "public",
  ).length;
  // A refresh that fails leaves the form, and what is typed in it.
  const failed = config.error != null && (
    <LoadError error={config.error} retry={config.retry} />
  );
  if (!config.loaded) return failed || <SettingRowsLoading />;
  return (
    <section class="configuration">
      {failed}
      {config.problem && (
        <p class="group-footer setting-error" role="alert">
          {config.problem}
        </p>
      )}
      {config.restart.length > 0 && (
        <RestartNotice
          key={config.restart.join()}
          keys={config.restart.map((key) => find(key)?.label || key)}
          host={config.restart.some((key) => find(key)?.category === "web")}
          running={sessions.filter((item) => item.generation).length}
          folder={scope === "project" ? folder : undefined}
        />
      )}
      <div class="group-block configuration-bar">
        <Input
          type="search"
          aria-label="Find a setting"
          placeholder="Find a setting"
          value={query}
          disabled={asFile}
          onInput={(event) => setQuery(event.currentTarget.value)}
        />
        <Button variant="quiet" onClick={() => setAsFile(!asFile)}>
          {asFile ? "Show the form" : "Show the file"}
        </Button>
      </div>
      {asFile && (
        <Group
          title="settings.json"
          footer={
            <>
              <code>{config.file}</code> as it stands, with keys and other
              secrets hidden. Edit it in an editor:{" "}
              <code>settings.schema.json</code> beside it completes and checks
              every setting, and a change is taken up at the next message.
            </>
          }
        >
          <pre class="configuration-file">
            <code>{JSON.stringify(config.document ?? {}, null, 2)}</code>
          </pre>
        </Group>
      )}
      {!asFile && INTRO[scope] && !needle && (
        <p class="group-footer">{INTRO[scope]}</p>
      )}
      {!asFile &&
        config.categories.map(({ id, label }) => {
          const own = listed.filter((setting) => setting.category === id);
          return (
            own.length > 0 && (
              <Group key={id} title={label}>
                {own.map((setting) => (
                  <SettingField
                    key={setting.name}
                    setting={setting}
                    scope={scope}
                    folder={folder}
                    find={find}
                    note={note(
                      setting,
                      scope,
                      find,
                      config.restart.includes(setting.name),
                    )}
                    error={
                      config.failed?.key === setting.name
                        ? String(
                            (config.failed.error as Error)?.message ||
                              config.failed.error,
                          )
                        : undefined
                    }
                    disabled={!online}
                    online={online}
                    save={config.save}
                  />
                ))}
              </Group>
            )
          );
        })}
      {!asFile && needle && !listed.length && (
        <p class="group-footer">No setting matches.</p>
      )}
      {!asFile && !needle && config.file && (
        <p class="group-footer">
          Saved in <code>{config.file}</code>, which an editor can open too:{" "}
          <code>settings.schema.json</code> beside it names every setting.
        </p>
      )}
      {!asFile && !needle && (
        <Group>
          <Row
            label={whole.reset}
            detail={held ? plural(held, "changed setting") : whole.none}
            destructive
            disabled={!held || !online}
            onClick={() => setConfirm(true)}
          />
        </Group>
      )}
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
    </section>
  );
}
