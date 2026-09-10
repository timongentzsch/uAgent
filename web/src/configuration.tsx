import { count } from "./quantities.ts";
import type {
  JSONValue,
  ConfigSetting,
  ConfigChange,
  Configuration as ConfigurationData,
  Session,
} from "./types.ts";
import "./configuration.css";
import { useEffect, useState } from "preact/hooks";
import { command } from "./store.ts";
import { Select, Skeleton, LoadError } from "./ui.tsx";

const stringify = (value?: JSONValue) =>
  value == null
    ? ""
    : typeof value === "boolean"
      ? value
        ? "1"
        : "0"
      : String(value);
function Setting({
  setting,
  scope,
  save,
  busy,
}: {
  setting: ConfigSetting;
  scope: string;
  save: (change: ConfigChange) => Promise<boolean>;
  busy: boolean;
}) {
  const configured =
    setting.type === "boolean"
      ? ["1", "true", "yes", "on"].includes(
          stringify(setting.value).toLowerCase(),
        )
        ? "1"
        : "0"
      : stringify(setting.value);
  const [value, setValue] = useState(configured);
  useEffect(() => setValue(configured), [configured]);
  const secret = setting.sensitivity !== "public";
  const editable = setting.scopes.includes(scope);
  const id = setting.name;
  return (
    <div class="config-row">
      <label htmlFor={id}>
        <strong>{id}</strong>
        <small>{setting.description}</small>
      </label>
      <small class="muted">
        {setting.source} · {setting.takes_effect.replaceAll("_", " ")}
        {setting.active !== undefined &&
          ` · active: ${stringify(setting.active)}`}
      </small>
      <div class="config-value">
        {setting.type === "boolean" ? (
          <Select
            id={id}
            value={value}
            disabled={!editable || busy}
            onChange={(event) => setValue(event.currentTarget.value)}
          >
            <option value="0">Off</option>
            <option value="1">On</option>
          </Select>
        ) : (
          <input
            id={id}
            type={
              secret
                ? "password"
                : ["integer", "number"].includes(setting.type)
                  ? "number"
                  : "text"
            }
            step={setting.type === "integer" ? "1" : "any"}
            min={
              Number.isSafeInteger(setting.minimum)
                ? setting.minimum
                : undefined
            }
            max={
              Number.isSafeInteger(setting.maximum)
                ? setting.maximum
                : undefined
            }
            value={value}
            placeholder={
              secret
                ? setting.set
                  ? "Configured · enter replacement"
                  : "Not configured"
                : `Default: ${stringify(setting.default)}`
            }
            disabled={!editable || busy}
            onInput={(event) => setValue(event.currentTarget.value)}
          />
        )}
        <button
          disabled={
            !editable || busy || (secret ? !value : value === configured)
          }
          onClick={async () => {
            if ((await save({ key: id, value })) && secret) setValue("");
          }}
        >
          Apply
        </button>
        <button
          disabled={!editable || busy || !setting.set}
          onClick={() => save({ key: id, unset: true })}
        >
          Reset
        </button>
      </div>
    </div>
  );
}
export default function Configuration({
  session,
  online,
}: {
  session?: Session;
  online: boolean;
}) {
  const [data, setData] = useState<ConfigurationData | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  const [scope, setScope] = useState("user");
  const [query, setQuery] = useState("");
  const [busy, setBusy] = useState(false);
  const [receipt, setReceipt] = useState("");
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
  async function save(change: ConfigChange) {
    setBusy(true);
    setReceipt("");
    setError(null);
    try {
      const response = await command("config", target, {
        operation: "apply",
        scope,
        changes: [change],
      });
      if (!response.pending) {
        setData(response.result);
        setReceipt(
          response.result.effects
            .map(
              (effect) =>
                `${effect.key}: ${effect.effect.replaceAll("_", " ")}`,
            )
            .join("\n"),
        );
        return true;
      }
    } catch (error) {
      setError(error);
    } finally {
      setBusy(false);
    }
    return false;
  }
  const groups = new Map<string, ConfigSetting[]>();
  for (const setting of data?.settings || []) {
    if (
      !`${setting.name} ${setting.description}`
        .toLowerCase()
        .includes(query.toLowerCase())
    )
      continue;
    if (!groups.has(setting.category)) groups.set(setting.category, []);
    groups.get(setting.category)!.push(setting);
  }
  return (
    <section class="configuration">
      <label>
        Change scope
        <Select
          value={scope}
          onChange={(event) => setScope(event.currentTarget.value)}
        >
          <option value="user">User defaults</option>
          <option value="project" disabled={!target || !data?.project_trusted}>
            This project
          </option>
        </Select>
      </label>
      <label>
        Find a setting
        <input
          type="search"
          value={query}
          onInput={(event) => setQuery(event.currentTarget.value)}
        />
      </label>
      {receipt && <p role="status">{receipt}</p>}
      {error ? (
        <LoadError error={error} retry={() => setAttempt(attempt + 1)} />
      ) : (
        !data && (
          <Skeleton
            className="form-skeleton"
            rows={5}
            label="Loading configuration…"
          />
        )
      )}
      {[...groups].map(([category, settings]) => (
        <details key={category} open={!!query}>
          <summary>
            {category} · {count(settings.length)}
          </summary>
          {settings.map((setting) => (
            <Setting
              key={setting.name}
              setting={setting}
              scope={scope}
              busy={
                busy ||
                !online ||
                (scope === "project" && (!target || !data?.project_trusted))
              }
              save={save}
            />
          ))}
        </details>
      ))}
    </section>
  );
}
