import { plural } from "../../../shared/quantities.ts";
import { Group, Row, ValueSelect } from "../../../shared/ui.tsx";
import { ZoomSlider } from "../../../shared/zoom-slider.tsx";
import { defaultTimePrefs, type TimePrefs } from "../../../shared/time.ts";
import { devicePrefs } from "../../../shared/device-prefs.ts";
import { useStored } from "../../../shared/storage.ts";

const { theme: themePref, motion: motionPref, zoom: zoomPref } = devicePrefs;
const TIMESTAMPS: Record<TimePrefs["style"], string> = {
  smart: "Time today, the date when older.",
  relative: "How long ago, e.g. 5 min ago.",
  absolute: "The full date and time.",
};

export function DisplayPane() {
  const [theme, setTheme] = useStored(themePref);
  const [motion, setMotion] = useStored(motionPref);
  const [timePrefs, setTimePrefs] = useStored(devicePrefs.time);
  const [zoom, setZoom] = useStored(zoomPref);
  // How many differ from their defaults.
  const changed =
    Number(theme !== themePref.fallback) +
    Number(motion !== motionPref.fallback) +
    Number(zoom !== zoomPref.fallback) +
    Number(timePrefs.clock !== defaultTimePrefs.clock) +
    Number(timePrefs.style !== defaultTimePrefs.style);
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
          reset={() => setZoom(zoomPref.fallback)}
        />
      </Row>
      <Row
        label="Reset display"
        detail={
          changed
            ? plural(changed, "changed setting")
            : "Everything is at its default"
        }
        destructive
        disabled={!changed}
        onClick={() => {
          setTheme(themePref.fallback);
          setMotion(motionPref.fallback);
          setZoom(zoomPref.fallback);
          setTimePrefs(defaultTimePrefs);
        }}
      />
    </Group>
  );
}
