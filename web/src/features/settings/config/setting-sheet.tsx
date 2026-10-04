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
import { reason, secret, shown } from "./facts.ts";

const APPLIES: Record<string, string> = {
  "next-user-turn": "applies from your next message",
  "restart-required": "applies after a restart",
};

// The one editor: what the setting is for, its value, why that applies, and
// two ways out. Saving an empty value is the same as Use default.
export function SettingSheet({
  setting,
  find,
  busy,
  error,
  save,
  close,
}: {
  setting: ConfigSetting;
  find: (name: string) => ConfigSetting | undefined;
  busy: boolean;
  error: unknown;
  save: (change: ConfigChange) => Promise<boolean>;
  close: () => void;
}) {
  const hidden = secret(setting);
  const [draft, setDraft] = useState(hidden ? "" : shown(setting.set?.user));
  const numeric = ["integer", "number"].includes(setting.type);
  const commit = async (value: string) => {
    const change = value
      ? { key: setting.name, value }
      : { key: setting.name, unset: true };
    if ((!value && setting.set?.user === undefined) || (await save(change)))
      close();
  };
  const line = reason(setting, find);
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
        ) : setting.choices?.length ? (
          <Select
            aria-label={setting.label}
            value={draft}
            onChange={(event) => setDraft(event.currentTarget.value)}
          >
            <option value="">Default</option>
            {setting.choices.map((choice) => (
              <option key={choice} value={choice}>
                {choice}
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
              hidden
                ? setting.set?.user
                  ? "Set · enter a replacement"
                  : "Not set"
                : shown(setting.effective) || "Not set"
            }
            onInput={(event) => setDraft(event.currentTarget.value)}
          />
        )}
        {line && <p class="muted">{line}</p>}
        {error != null && <LoadError error={error} />}
        {!setting.locked && (
          <Actions>
            <Button
              disabled={busy || setting.set?.user === undefined}
              onClick={() => void commit("")}
            >
              Use default
            </Button>
            <Button variant="primary" type="submit" busy={busy}>
              Save
            </Button>
          </Actions>
        )}
        <small class="muted">
          <code>{setting.name}</code> · {APPLIES[setting.takes_effect]}
        </small>
      </form>
    </Modal>
  );
}
