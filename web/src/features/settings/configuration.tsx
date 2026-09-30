import { count, plural } from "../../shared/quantities.ts";
import type {
  JSONValue,
  ConfigSetting,
  ConfigChange,
  Configuration as ConfigurationData,
  Session,
} from "../../shared/types.ts";
import { useEffect, useRef, useState } from "preact/hooks";
import { SettingRowsLoading } from "./loading.tsx";
import { command } from "../../state/api.ts";
import {
  Actions,
  Button,
  Group,
  Input,
  LoadError,
  Modal,
  Row,
  SettingRow,
  Switch,
  ValueSelect,
} from "../../shared/ui.tsx";

const SAVED_MS = 1_500;
const RESTART = "needs a restart";

const stringify = (value?: JSONValue) =>
  value == null
    ? ""
    : typeof value === "boolean"
      ? value
        ? "1"
        : "0"
      : String(value);
const normalize = (setting: ConfigSetting, value?: JSONValue) =>
  setting.type === "boolean"
    ? ["1", "true", "yes", "on"].includes(stringify(value).toLowerCase())
      ? "1"
      : "0"
    : stringify(value);
// A value set in the environment or on the command line wins over files.
const lockedBy = (setting: ConfigSetting) =>
  setting.source === "environment"
    ? "Set by the environment; change it there."
    : setting.source === "cli"
      ? "Set on the command line; change it there."
      : undefined;
// What applies in a scope: a lock's value, the scope's own, else what it
// inherits (a project from User defaults, User defaults from the default).
const applied = (setting: ConfigSetting, scope: "user" | "project") =>
  normalize(
    setting,
    lockedBy(setting)
      ? setting.value
      : (setting[scope] ??
          (scope === "project" ? setting.user : undefined) ??
          setting.default),
  );
// What an empty value falls back to: another setting, followed to the value
// that applies, or the phrase the registry gives.
function fallbackLabel(
  setting: ConfigSetting,
  scope: "user" | "project",
  find: (name: string) => ConfigSetting | undefined,
  depth = 0,
): string {
  const other = setting.fallback ? find(setting.fallback) : undefined;
  if (!other || depth > 3) return setting.fallback || "";
  const value =
    applied(other, scope) || fallbackLabel(other, scope, find, depth + 1);
  return value ? `${value} (${other.name})` : `Same as ${other.name}`;
}
// Settings of the web host itself apply when the host restarts, not a
// conversation.
const hostSetting = (setting?: ConfigSetting) => setting?.category === "web";

// One registry setting in the edited scope: it shows what applies there,
// its own value or the one it inherits, and Reset only while it overrides.
// Saving the inherited value removes the override instead of pinning it.
function Setting({
  setting,
  scope,
  save,
  busy,
  find,
}: {
  setting: ConfigSetting;
  scope: "user" | "project";
  save: (change: ConfigChange) => Promise<boolean>;
  busy: boolean;
  find: (name: string) => ConfigSetting | undefined;
}) {
  const secret = setting.sensitivity !== "public";
  const own = setting[scope];
  const inherited = normalize(
    setting,
    scope === "project" ? (setting.user ?? setting.default) : setting.default,
  );
  const locked = lockedBy(setting);
  const configured = secret
    ? ""
    : locked || own === undefined
      ? applied(setting, scope)
      : normalize(setting, own);
  // An empty value still says what applies instead of looking unset.
  const fallback = secret ? "" : fallbackLabel(setting, scope, find);
  // An edit in progress; otherwise the row shows the configured value, so a
  // save or a change elsewhere never overwrites what is being typed.
  const [draft, setDraft] = useState<string | null>(null);
  const value = draft ?? configured;
  const [saved, setSaved] = useState(false);
  useEffect(() => {
    if (!saved) return;
    const timer = setTimeout(() => setSaved(false), SAVED_MS);
    return () => clearTimeout(timer);
  }, [saved]);
  const disabled = busy || !!locked || !setting.scopes.includes(scope);
  const id = setting.name;
  // One save at a time per row: disabling a focused field blurs it, and
  // that blur must not submit the same value again.
  const saving = useRef(false);
  const apply = async (next: string) => {
    if (disabled || saving.current) return;
    if (secret ? !next : next === configured) return setDraft(null);
    const unset = !secret && next === inherited;
    if (unset && own === undefined) return;
    saving.current = true;
    try {
      if (await save(unset ? { key: id, unset } : { key: id, value: next })) {
        setSaved(true);
        setDraft(null);
      }
    } finally {
      saving.current = false;
    }
  };
  return (
    <SettingRow
      name={id}
      label={<code>{id}</code>}
      htmlFor={id}
      detail={setting.description}
      overridden={own !== undefined}
      locked={locked}
      saved={saved}
      disabled={disabled}
      reset={() => {
        setDraft(null);
        void save({ key: id, unset: true });
      }}
    >
      {setting.type === "boolean" ? (
        <Switch
          label={id}
          checked={value === "1"}
          disabled={disabled}
          onChange={(on) => {
            setDraft(on ? "1" : "0");
            void apply(on ? "1" : "0");
          }}
        />
      ) : setting.choices?.length ? (
        <ValueSelect
          id={id}
          value={value}
          disabled={disabled}
          onChange={(event) => {
            setDraft(event.currentTarget.value);
            void apply(event.currentTarget.value);
          }}
        >
          {!setting.choices.includes(inherited) && (
            <option value={inherited}>
              {fallback ? `Default · ${fallback}` : "Default"}
            </option>
          )}
          {setting.choices.map((choice) => (
            <option key={choice} value={choice}>
              {choice}
            </option>
          ))}
        </ValueSelect>
      ) : (
        <Input
          id={id}
          type={
            secret
              ? "password"
              : ["integer", "number"].includes(setting.type)
                ? "number"
                : "text"
          }
          enterkeyhint="done"
          step={setting.type === "integer" ? "1" : "any"}
          min={
            Number.isSafeInteger(setting.minimum) ? setting.minimum : undefined
          }
          max={
            Number.isSafeInteger(setting.maximum) ? setting.maximum : undefined
          }
          value={value}
          placeholder={
            secret
              ? locked
                ? "Configured"
                : own
                  ? "Configured · enter replacement"
                  : "Not set"
              : fallback || "Not set"
          }
          disabled={disabled}
          onInput={(event) => setDraft(event.currentTarget.value)}
          onKeyDown={(event) => {
            if (event.key === "Enter") void apply(value);
          }}
          onBlur={(event) => {
            // Leaving the field for its own Reset discards the edit.
            const next = event.relatedTarget as Element | null;
            if (next?.closest(".setting-reset")) return;
            void apply(value);
          }}
        />
      )}
    </SettingRow>
  );
}

// A saved change that only a restart applies offers that restart: the
// running conversations it reaches, or the web host for its own settings.
function RestartNotice({
  keys,
  host,
  running,
  cwd,
}: {
  keys: string[];
  host: boolean;
  running: number;
  cwd?: string;
}) {
  const [busy, setBusy] = useState(false);
  const [done, setDone] = useState("");
  const [error, setError] = useState<unknown>(null);
  const restart = async () => {
    setBusy(true);
    setError(null);
    try {
      if (host) {
        await command("restart_host", null);
        setDone("Restarting the web host…");
        return;
      }
      const response = await command(
        "restart_conversations",
        null,
        cwd ? { cwd } : {},
      );
      if (response.pending) return;
      const { restarting, deferred } = response.result;
      setDone(
        [
          restarting && `Restarted ${plural(restarting, "conversation")}`,
          deferred && `${plural(deferred, "busy conversation")} after its turn`,
        ]
          .filter(Boolean)
          .join(" · ") || "No conversation was running",
      );
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(false);
    }
  };
  const applies = `Applies ${keys.join(", ")}.`;
  return (
    <Group
      title="Restart to apply"
      footer={
        host
          ? "Conversations keep running; this page reconnects by itself."
          : "New conversations use it already. Running ones keep their history."
      }
    >
      {done ? (
        <Row label={done} detail={applies} />
      ) : host || running > 0 ? (
        <Row
          label={
            host
              ? "Restart web host"
              : `Restart ${plural(running, "running conversation")}`
          }
          detail={applies}
          disabled={busy}
          onClick={() => void restart()}
        />
      ) : (
        <Row label="No conversation is running" detail={applies} />
      )}
      {error !== null && <LoadError error={error} />}
    </Group>
  );
}

// With a filter, a settings section embeds just its settings as one flat
// list; without one, every setting is searchable by category, and the
// scope can be reset as a whole.
export default function Configuration({
  session,
  online,
  filter,
  sessions = [],
  display,
}: {
  session?: Session;
  online: boolean;
  filter?: (setting: ConfigSetting) => boolean;
  sessions?: Session[];
  // This device's own settings, reset together with the user scope.
  display?: { changed: number; reset: () => void };
}) {
  const [data, setData] = useState<ConfigurationData | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  const [scope, setScope] = useState<"user" | "project">("user");
  const [query, setQuery] = useState("");
  const [busy, setBusy] = useState(false);
  const [restart, setRestart] = useState<string[]>([]);
  const [shadowed, setShadowed] = useState<string[]>([]);
  const [confirm, setConfirm] = useState(false);
  const target = session?.generation && !session.turn_active ? session : null;
  useEffect(() => {
    let active = true;
    setError(null);
    command("config", target, { operation: "get" })
      .then((response) => {
        if (!active) return;
        if (response.pending)
          throw new Error("Configuration is still loading. Try again shortly.");
        setData(response.result);
      })
      .catch((failure) => {
        if (active) setError(failure);
      });
    return () => {
      active = false;
    };
  }, [target?.id, attempt]);
  async function run(fields: Record<string, unknown>) {
    setBusy(true);
    setError(null);
    try {
      const response = await command("config", target, { scope, ...fields });
      if (response.pending) return false;
      const result: ConfigurationData = response.result;
      setData(result);
      const keys = (effect: string) =>
        result.effects
          .filter((item) => item.effect === effect)
          .map((item) => item.key);
      // Collected while settings are open, so several saves offer one restart.
      setRestart((current) => [...new Set([...current, ...keys(RESTART)])]);
      setShadowed(keys("saved, but a higher layer keeps winning"));
      return true;
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(false);
    }
    return false;
  }
  const save = (change: ConfigChange) =>
    run({ operation: "apply", changes: [change] });
  const settings = data?.settings || [];
  const byName = new Map(settings.map((setting) => [setting.name, setting]));
  const groups = new Map<string, ConfigSetting[]>();
  for (const setting of settings) {
    if (filter && !filter(setting)) continue;
    if (
      !`${setting.name} ${setting.description}`
        .toLowerCase()
        .includes(query.toLowerCase())
    )
      continue;
    if (!groups.has(setting.category)) groups.set(setting.category, []);
    groups.get(setting.category)!.push(setting);
  }
  // Reset all covers the scope's public settings; keys stay.
  const changed =
    settings.filter(
      (setting) =>
        setting.sensitivity === "public" && setting[scope] !== undefined,
    ).length + (scope === "user" ? display?.changed || 0 : 0);
  const host = restart.some((key) => hostSetting(byName.get(key)));
  const running = sessions.filter(
    (item) =>
      item.generation && (scope === "user" || item.cwd === session?.cwd),
  ).length;
  const projectLocked =
    scope === "project" && (!target || !data?.project_trusted);
  return (
    <section class="configuration">
      {!filter && (
        <Group
          title="Scope"
          footer="Project values apply to this repository and override your defaults."
        >
          <Row label="Change scope">
            <ValueSelect
              aria-label="Change scope"
              value={scope}
              onChange={(event) =>
                setScope(event.currentTarget.value as "user" | "project")
              }
            >
              <option value="user">User defaults</option>
              <option
                value="project"
                disabled={!target || !data?.project_trusted}
              >
                This project
              </option>
            </ValueSelect>
          </Row>
          <div class="group-block">
            <Input
              type="search"
              aria-label="Find a setting"
              placeholder="Find a setting"
              value={query}
              onInput={(event) => setQuery(event.currentTarget.value)}
            />
          </div>
        </Group>
      )}
      {restart.length > 0 && (
        <RestartNotice
          key={restart.join()}
          keys={restart}
          host={host}
          running={running}
          cwd={scope === "project" ? session?.cwd : undefined}
        />
      )}
      {shadowed.length > 0 && (
        <p class="group-footer" role="status">
          {shadowed.join(", ")}: saved, but a value set in the environment or on
          the command line keeps winning.
        </p>
      )}
      {error ? (
        <LoadError error={error} retry={() => setAttempt(attempt + 1)} />
      ) : (
        !data && <SettingRowsLoading />
      )}
      {[...groups].map(([category, settings]) => (
        <Group
          key={category}
          title={filter ? undefined : `${category} · ${count(settings.length)}`}
        >
          {settings.map((setting) => (
            <Setting
              key={`${scope}:${setting.name}`}
              setting={setting}
              scope={scope}
              busy={busy || !online || projectLocked}
              save={save}
              find={(name) => byName.get(name)}
            />
          ))}
        </Group>
      ))}
      {!filter && data && (
        <Group>
          <Row
            label="Reset all to defaults"
            detail={
              changed
                ? `${plural(changed, "changed setting")} in ${scope === "user" ? "User defaults" : "this project"}`
                : "Everything is at its default"
            }
            destructive
            disabled={!changed || busy || !online || projectLocked}
            onClick={() => setConfirm(true)}
          />
        </Group>
      )}
      {confirm && (
        <Modal title="Reset all to defaults" close={() => setConfirm(false)}>
          <p>
            {scope === "user"
              ? "Every changed setting in User defaults and this device's display settings returns to its default."
              : "Every setting this project changes returns to what it inherits."}{" "}
            API keys and other secrets are kept.
          </p>
          <Actions>
            <Button onClick={() => setConfirm(false)}>Cancel</Button>
            <Button
              variant="destructive"
              busy={busy}
              onClick={async () => {
                if (await run({ operation: "reset" })) {
                  if (scope === "user") display?.reset();
                  setConfirm(false);
                }
              }}
            >
              Reset all
            </Button>
          </Actions>
        </Modal>
      )}
    </section>
  );
}
