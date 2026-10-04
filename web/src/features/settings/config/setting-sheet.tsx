import { useState } from "preact/hooks";
import type { ConfigChange, ConfigSetting } from "../../../shared/types.ts";
import {
  Actions,
  Button,
  Input,
  LoadError,
  Modal,
  Select,
} from "../../../shared/ui.tsx";
import {
  inherited,
  origin,
  raw,
  SCOPE,
  secret,
  shown,
  type SavedScope,
} from "./facts.ts";

const APPLIES: Record<string, string> = {
  "next-user-turn": "applies from your next message",
  "restart-required": "applies after a restart",
};

// The one editor: what the setting is for, the value `scope` holds, where
// the one in effect comes from, and two ways out. Saving an empty value is
// the same as unsetting it there.
export function SettingSheet({
  setting,
  scope,
  find,
  busy,
  error,
  save,
  close,
}: {
  setting: ConfigSetting;
  scope: SavedScope;
  find: (name: string) => ConfigSetting | undefined;
  busy: boolean;
  error: unknown;
  save: (change: ConfigChange) => Promise<boolean>;
  close: () => void;
}) {
  const hidden = secret(setting);
  const own = setting.set?.[scope];
  const [draft, setDraft] = useState(hidden ? "" : raw(own));
  // What unsetting here falls back to, named: never a bare "default".
  const below = inherited(setting, scope, find);
  const choices: [string, string][] =
    setting.type === "boolean"
      ? [
          ["1", "On"],
          ["0", "Off"],
        ]
      : (setting.choices || []).map((choice) => [choice, choice]);
  const numeric = ["integer", "number"].includes(setting.type);
  const commit = async (value: string) => {
    const change = value
      ? { key: setting.name, value }
      : { key: setting.name, unset: true };
    if ((!value && own === undefined) || (await save(change))) close();
  };
  const line = origin(setting, find);
  return (
    <Modal title={setting.label} layout="sheet" size="narrow" close={close}>
      <form
        class="setting-sheet"
        onSubmit={(event) => {
          event.preventDefault();
          void commit(draft.trim());
        }}
      >
        <p>{setting.purpose || setting.description}</p>
        {setting.locked ? (
          <output>{shown(setting.effective)}</output>
        ) : choices.length ? (
          <Select
            aria-label={setting.label}
            value={draft}
            onChange={(event) => setDraft(event.currentTarget.value)}
          >
            <option value="">
              {scope === "user" ? "Built in" : "Same as all conversations"}:{" "}
              {below}
            </option>
            {choices.map(([value, label]) => (
              <option key={value} value={value}>
                {label}
              </option>
            ))}
          </Select>
        ) : (
          <Input
            aria-label={setting.label}
            type={hidden ? "password" : numeric ? "number" : "text"}
            enterkeyhint="done"
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
            value={draft}
            placeholder={
              hidden ? (own ? "Set · enter a replacement" : "Not set") : below
            }
            onInput={(event) => setDraft(event.currentTarget.value)}
          />
        )}
        {line && <p class="muted">{line}</p>}
        {error != null && <LoadError error={error} />}
        {!setting.locked && (
          <Actions>
            <Button
              disabled={busy || own === undefined}
              onClick={() => void commit("")}
            >
              {scope === "user" ? "Use default" : "Remove override"}
            </Button>
            <Button variant="primary" type="submit" busy={busy}>
              Save
            </Button>
          </Actions>
        )}
        <small class="muted">
          <code>{setting.name}</code> · saved for {SCOPE[scope].toLowerCase()} ·{" "}
          {APPLIES[setting.takes_effect]}
        </small>
      </form>
    </Modal>
  );
}
