import { RotateCcw } from "lucide-preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { permissionLabel, permissionLabels } from "../../../shared/display.ts";
import type { ConfigChange, ConfigSetting } from "../../../shared/types.ts";
import { SheetButton } from "../../../shared/sheet.tsx";
import {
  IconButton,
  Input,
  Row,
  Switch,
  Textarea,
  ValueSelect,
} from "../../../shared/ui.tsx";
import ModelPicker from "../../composer/model-picker.tsx";
import { inherited, raw, secret, shown, type SavedScope } from "./facts.ts";

// One setting, edited where it is listed. A switch and a choice apply as
// they change; text and numbers keep a draft and are saved on leaving the
// field or Enter, so nothing half-typed is ever saved. Putting it back to
// what applies without it is its own action.
export function SettingField({
  setting,
  scope,
  folder,
  find,
  note,
  error,
  disabled,
  online,
  save,
}: {
  setting: ConfigSetting;
  scope: SavedScope;
  folder?: string;
  find: (name: string) => ConfigSetting | undefined;
  // Who decides instead, what it overrides, or that a restart is due.
  note?: string;
  // Why its last save was refused.
  error?: string;
  disabled: boolean;
  online: boolean;
  save: (change: ConfigChange) => Promise<boolean>;
}) {
  const hidden = secret(setting);
  const own = setting.set?.[scope];
  const held = hidden ? "" : raw(own);
  const [draft, setDraft] = useState(held);
  // What is saved changed under the field (a save, a reset, another
  // browser): show it, unless something typed here is still unsaved.
  const [edited, setEdited] = useState(false);
  useEffect(() => {
    if (!edited) setDraft(held);
  }, [held]);
  const below = inherited(setting, scope, find);
  // A mode is called what it is called everywhere else.
  const named = (choice: string) =>
    setting.name === "UAGENT_APPROVAL" && choice in permissionLabels
      ? permissionLabel(choice)
      : choice;
  const placeholder = hidden
    ? own
      ? "Set · enter a replacement"
      : "Not set"
    : below;
  const set = (value: string) =>
    save(
      value ? { key: setting.name, value } : { key: setting.name, unset: true },
    );
  // The latest of each, for a save that answers after something newer.
  const typed = useRef(draft);
  typed.current = draft;
  // A switch or a choice shows what was just picked until its save answers.
  const [picked, setPicked] = useState<string>();
  const picks = useRef(0);
  const pick = async (value: string) => {
    const mine = ++picks.current;
    setPicked(value);
    await set(value);
    if (mine === picks.current) setPicked(undefined);
  };
  // Saved, the draft is settled, unless more was typed meanwhile.
  const settle = async (value: string) => {
    if ((await set(value)) && typed.current.trim() === value) setEdited(false);
  };
  const commit = () => {
    const value = draft.trim();
    if (!edited || value === held) return setEdited(false);
    void settle(value);
  };
  const keys = (event: KeyboardEvent) => {
    // Takes back what was typed, and only that: the dialog stays.
    if (event.key === "Escape" && edited) {
      event.preventDefault();
      event.stopPropagation();
      setDraft(held);
      setEdited(false);
    }
  };
  const text = {
    "aria-label": setting.label,
    value: draft,
    disabled,
    onInput: (event: { currentTarget: { value: string } }) => {
      setDraft(event.currentTarget.value);
      setEdited(true);
    },
    onBlur: commit,
    onKeyDown: keys,
  };
  const control = setting.locked ? (
    <span class="setting-summary">{shown(setting.effective) || "Set"}</span>
  ) : setting.type === "boolean" ? (
    <Switch
      label={setting.label}
      checked={
        picked !== undefined
          ? picked === "1"
          : (own ?? setting.set?.user ?? setting.default) === true
      }
      disabled={disabled}
      onChange={(on) => void pick(on ? "1" : "0")}
    />
  ) : setting.choices?.length ? (
    <ValueSelect
      aria-label={setting.label}
      value={picked ?? held}
      disabled={disabled}
      onChange={(event) => void pick(event.currentTarget.value)}
    >
      <option value="">{named(below)}</option>
      {setting.choices.map((choice) => (
        <option key={choice} value={choice}>
          {named(choice)}
        </option>
      ))}
    </ValueSelect>
  ) : setting.name.endsWith("_MODEL") ? (
    <SheetButton
      label={setting.label}
      title="Model, variant and effort"
      disabled={disabled}
      trigger={
        <span class="setting-summary" data-default={held ? undefined : ""}>
          {held || below}
        </span>
      }
    >
      {(close) => (
        <ModelPicker
          session={{ id: setting.name, cwd: folder }}
          selection={held}
          save={set}
          close={close}
          online={online}
          running={false}
        />
      )}
    </SheetButton>
  ) : setting.sensitivity === "composite-secret" ? (
    <Textarea {...text} rows={3} placeholder={placeholder} spellcheck={false} />
  ) : (
    <Input
      {...text}
      type={
        hidden
          ? "password"
          : ["integer", "number"].includes(setting.type)
            ? "number"
            : "text"
      }
      enterkeyhint="done"
      step={setting.type === "integer" ? "1" : "any"}
      min={Number.isSafeInteger(setting.minimum) ? setting.minimum : undefined}
      max={Number.isSafeInteger(setting.maximum) ? setting.maximum : undefined}
      placeholder={placeholder}
      onKeyDown={(event) => {
        if (event.key === "Enter") event.currentTarget.blur();
        keys(event);
      }}
    />
  );
  return (
    <div
      class="setting-field"
      data-setting={setting.name}
      // A value that may be long gets its own line on a phone.
      data-wide={
        setting.type === "boolean" || setting.choices?.length ? undefined : ""
      }
    >
      <Row
        label={setting.label}
        detail={
          error ? (
            <span class="setting-error" role="alert">
              {error}
            </span>
          ) : (
            note || setting.purpose || setting.description
          )
        }
      >
        {control}
        {!setting.locked && own !== undefined && (
          <IconButton
            label={`Reset ${setting.label}`}
            disabled={disabled}
            onClick={async () => {
              if (await set("")) {
                setDraft("");
                setEdited(false);
              }
            }}
          >
            <RotateCcw />
          </IconButton>
        )}
      </Row>
    </div>
  );
}
