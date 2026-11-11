import { useState } from "preact/hooks";
import { Actions, Button, Switch } from "../../shared/ui.tsx";
import { Input } from "../../shared/form-controls.tsx";
import type { Tokens } from "./tokens.ts";

// A page's Tweaks: the levers it declares, and the theme of the design
// system it links. What is tried shows in the page at once; Keep writes it
// into the draft for the agent, and Reset draws the page as it was written.

export interface Tweak {
  editor?: string;
  default?: unknown;
  options?: unknown[];
  min?: number;
  max?: number;
  step?: number;
  // Shown beside the value, never applied: the page's CSS gives the unit.
  unit?: string;
}

// What was tried: the page's levers, and the theme's tokens.
const NONE = {
  tweaks: {} as Record<string, unknown>,
  theme: {} as Record<string, unknown>,
};
// "name value, name value": how they are written for the agent.
const pairs = (values: Record<string, unknown>) =>
  Object.entries(values)
    .map(([name, value]) => `${name} ${value}`)
    .join(", ");

// A choice among a few, one pressed; colours as swatches.
function Options({
  options,
  value,
  pick,
  swatch,
}: {
  options: unknown[];
  value: unknown;
  pick: (option: unknown) => void;
  swatch?: boolean;
}) {
  return (
    <>
      {options.map(String).map((name, at) => (
        <Button
          key={name}
          variant="secondary"
          aria-pressed={value === options[at]}
          onClick={() => pick(options[at])}
          {...(swatch && {
            class: "page-swatch",
            "aria-label": name,
            style: { background: name },
          })}
        >
          {!swatch && name}
        </Button>
      ))}
    </>
  );
}

export default function Tweaks({
  open,
  path,
  declared,
  theme,
  system,
  tell,
  reload,
  addToChat,
}: {
  open: boolean;
  path: string;
  declared: [string, Tweak][];
  // The linked design system's tokens, and its tokens.css in the folder.
  theme?: Tokens;
  system: string;
  // Sets a lever or a token in the page.
  tell: (name: string, value: unknown) => void;
  reload: () => void;
  addToChat?: (text: string) => void;
}) {
  const [tried, setTried] = useState(NONE);
  const set = (group: keyof typeof NONE, name: string, value: unknown) => {
    setTried((all) => ({ ...all, [group]: { ...all[group], [name]: value } }));
    tell(name, value);
  };
  // Closed, it keeps what was tried.
  if (!open) return null;
  return (
    <aside class="page-tweaks" aria-label="Tweaks">
      {declared.map(([name, tweak]) => {
        const value = tried.tweaks[name] ?? tweak.default;
        const label = name.replace(/^--/, "");
        const color = tweak.editor === "color";
        return (
          <div key={name} class="page-tweak">
            <span class="small">
              {label}
              {!color && tweak.editor !== "boolean" && (
                <span class="muted">{` ${value ?? ""}${tweak.unit ?? ""}`}</span>
              )}
            </span>
            {color || tweak.editor === "enum" ? (
              <div class="segmented">
                <Options
                  options={tweak.options ?? []}
                  value={value}
                  pick={(option) => set("tweaks", name, option)}
                  swatch={color}
                />
                {color && (
                  <Input
                    type="color"
                    aria-label={`${label}, any colour`}
                    value={String(value ?? "")}
                    onInput={(event) =>
                      set("tweaks", name, event.currentTarget.value)
                    }
                  />
                )}
              </div>
            ) : tweak.editor === "boolean" ? (
              <Switch
                label={label}
                checked={!!value}
                onChange={(checked) => set("tweaks", name, checked)}
              />
            ) : (
              <Input
                type="range"
                aria-label={label}
                min={tweak.min ?? 0}
                max={tweak.max ?? 100}
                step={tweak.step ?? (tweak.editor === "int" ? 1 : 0.01)}
                value={Number(value ?? 0)}
                onInput={(event) =>
                  set("tweaks", name, Number(event.currentTarget.value))
                }
              />
            )}
          </div>
        );
      })}
      {theme && theme.colors.length > 0 && (
        <section class="page-theme" aria-label="Theme">
          <span class="small">
            Theme <span class="muted">{system}</span>
          </span>
          {theme.themes.length > 1 && (
            <div class="segmented">
              <Options
                options={theme.themes}
                value={tried.theme.theme ?? theme.themes[0]}
                pick={(id) => set("theme", "theme", id)}
              />
            </div>
          )}
          {theme.colors
            .filter((color) => /^#[0-9a-f]{6}$/i.test(color.value))
            .map((color) => (
              <label key={color.name} class="page-theme-token small">
                <Input
                  type="color"
                  value={String(tried.theme[`--${color.name}`] ?? color.value)}
                  onInput={(event) =>
                    set("theme", `--${color.name}`, event.currentTarget.value)
                  }
                />
                {color.name}
              </label>
            ))}
          {addToChat && Object.keys(tried.theme).length > 0 && (
            <Button
              onClick={() => {
                const { theme: id, ...colors } = tried.theme;
                addToChat(
                  `\`${system}\` theme ${id ?? theme.themes[0]}: ${pairs(colors)}`,
                );
              }}
            >
              Keep theme
            </Button>
          )}
        </section>
      )}
      <Actions>
        <Button
          variant="secondary"
          onClick={() => {
            setTried(NONE);
            reload();
          }}
        >
          Reset
        </Button>
        {addToChat && (
          <Button
            disabled={!Object.keys(tried.tweaks).length}
            onClick={() =>
              addToChat(`\`${path}\` tweaks: ${pairs(tried.tweaks)}`)
            }
          >
            Keep
          </Button>
        )}
      </Actions>
    </aside>
  );
}
