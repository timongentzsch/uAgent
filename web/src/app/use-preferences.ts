import { useEffect, useLayoutEffect, useState } from "preact/hooks";
import { applyMotion, applyTheme, applyZoom } from "../shared/layout.ts";
import { devicePrefs } from "../shared/device-prefs.ts";
import type { TimePrefs } from "../shared/time.ts";

// This browser's preferences as the shell holds them: read once, written
// back and applied to the document whenever one changes.
export function usePreferences() {
  const [zoom, setZoom] = useState(() => devicePrefs.zoom.read());
  const [timePrefs, setTimePrefs] = useState<TimePrefs>(devicePrefs.time.read);
  useEffect(() => devicePrefs.time.write(timePrefs), [timePrefs]);
  const [theme, setTheme] = useState(devicePrefs.theme.read);
  const [motion, setMotion] = useState(devicePrefs.motion.read);
  useEffect(() => {
    devicePrefs.zoom.write(zoom);
    applyZoom(zoom);
  }, [zoom]);
  useEffect(() => {
    devicePrefs.theme.write(theme);
    return applyTheme(theme);
  }, [theme]);
  useLayoutEffect(() => {
    devicePrefs.motion.write(motion);
    return applyMotion(motion);
  }, [motion]);
  return {
    zoom,
    setZoom,
    timePrefs,
    setTimePrefs,
    theme,
    setTheme,
    motion,
    setMotion,
  };
}
