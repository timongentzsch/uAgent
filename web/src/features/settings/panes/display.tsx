import { Group, Row, ValueSelect } from "../../../shared/ui.tsx";
import { ZoomSlider } from "../../../shared/zoom-slider.tsx";
import { defaultTimePrefs, type TimePrefs } from "../../../shared/time.ts";
import { devicePrefs } from "../../../shared/device-prefs.ts";
import { useSettings, type SettingsProps } from "../context.ts";

// This browser's display settings at their defaults.
const DEFAULTS = {
  theme: devicePrefs.theme.fallback,
  motion: devicePrefs.motion.fallback,
  zoom: devicePrefs.zoom.fallback,
};
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
  timePrefs,
}: SettingsProps) =>
  Number(theme !== DEFAULTS.theme) +
  Number(motion !== DEFAULTS.motion) +
  Number(zoom !== DEFAULTS.zoom) +
  Number(timePrefs.clock !== defaultTimePrefs.clock) +
  Number(timePrefs.style !== defaultTimePrefs.style);
export function resetDisplay({
  setTheme,
  setMotion,
  setZoom,
  setTimePrefs,
}: SettingsProps) {
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
  } = useSettings();
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
    <Group>
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
