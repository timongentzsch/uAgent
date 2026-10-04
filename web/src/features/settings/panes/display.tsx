import { Group, Row, ValueSelect } from "../../../shared/ui.tsx";
import { ZoomSlider } from "../../../shared/zoom-slider.tsx";
import { defaultTimePrefs, type TimePrefs } from "../../../shared/time.ts";
import { useSettings, type SettingsProps } from "../context.ts";
import { levelLabel } from "../../../shared/verbosity.ts";

// This device's display settings and their defaults, stored in the browser.
const DEFAULTS = {
  theme: "system",
  motion: "system",
  zoom: 100,
  detail: "",
} as const;
const TIMESTAMPS: Record<TimePrefs["style"], string> = {
  smart: "Time today, the date when older.",
  relative: "How long ago, e.g. 5 min ago.",
  absolute: "The full date and time.",
};

// How many differ from their defaults, and putting them all back: Advanced's
// "Reset all" counts and resets them with the host's settings.
export const displayChanged = ({
  theme,
  motion,
  zoom,
  deviceDetail,
  timePrefs,
}: SettingsProps) =>
  Number(deviceDetail !== DEFAULTS.detail) +
  Number(theme !== DEFAULTS.theme) +
  Number(motion !== DEFAULTS.motion) +
  Number(zoom !== DEFAULTS.zoom) +
  Number(timePrefs.clock !== defaultTimePrefs.clock) +
  Number(timePrefs.style !== defaultTimePrefs.style);
export function resetDisplay({
  setTheme,
  setMotion,
  setZoom,
  setDeviceDetail,
  setTimePrefs,
}: SettingsProps) {
  setDeviceDetail(DEFAULTS.detail);
  setTheme(DEFAULTS.theme);
  setMotion(DEFAULTS.motion);
  setZoom(DEFAULTS.zoom);
  setTimePrefs(defaultTimePrefs);
}

export function DisplayPane() {
  const {
    theme,
    setTheme,
    motion,
    setMotion,
    timePrefs,
    setTimePrefs,
    zoom,
    setZoom,
    deviceDetail,
    setDeviceDetail,
    catalogue,
  } = useSettings();
  const levels = Object.keys(catalogue.verbosity?.levels || {});
  // One row per choice: its name, its options, and where it is kept.
  const choice = (
    label: string,
    value: string,
    options: [string, string][],
    change: (value: string) => void,
    detail?: string,
  ) => (
    <Row label={label} detail={detail}>
      <ValueSelect
        aria-label={label}
        value={value}
        onChange={(event) => change(event.currentTarget.value)}
      >
        {options.map(([id, name]) => (
          <option key={id} value={id}>
            {name}
          </option>
        ))}
      </ValueSelect>
    </Row>
  );
  return (
    <Group title="Display" footer="Saved on this device.">
      {choice(
        "Appearance",
        theme,
        [
          ["system", "System"],
          ["dark", "Dark"],
          ["light", "Light"],
        ],
        setTheme,
      )}
      {choice(
        "Animations",
        motion,
        [
          ["system", "System"],
          ["off", "Off"],
        ],
        setMotion,
      )}
      {choice(
        "Clock",
        timePrefs.clock,
        [
          ["system", "System"],
          ["12", "12-hour"],
          ["24", "24-hour"],
        ],
        (clock) =>
          setTimePrefs({ ...timePrefs, clock: clock as TimePrefs["clock"] }),
      )}
      {choice(
        "Timestamps",
        timePrefs.style,
        [
          ["smart", "Smart"],
          ["relative", "Relative"],
          ["absolute", "Absolute"],
        ],
        (style) =>
          setTimePrefs({ ...timePrefs, style: style as TimePrefs["style"] }),
        TIMESTAMPS[timePrefs.style],
      )}
      {levels.length > 0 &&
        choice(
          "Detail on this device",
          levels.includes(deviceDetail) ? deviceDetail : DEFAULTS.detail,
          [
            [DEFAULTS.detail, "Follow global"],
            ...levels.map((level): [string, string] => [
              level,
              levelLabel(level),
            ]),
          ],
          setDeviceDetail,
          "How much of the agent's work a conversation shows here.",
        )}
      <Row
        label="Zoom"
        detail="Scales the entire interface, conversation included — like browser zoom."
      >
        <ZoomSlider
          zoom={zoom}
          change={setZoom}
          reset={() => setZoom(DEFAULTS.zoom)}
        />
      </Row>
    </Group>
  );
}
