// Mirrors CLI FmtDuration; timestamps and raw millisecond values stay exact.
export function duration(milliseconds?: number) {
  if (
    milliseconds === undefined ||
    !Number.isFinite(milliseconds) ||
    milliseconds < 0
  )
    return "Not recorded";
  if (milliseconds < 1000) return `${Math.round(milliseconds)}ms`;
  const seconds = milliseconds / 1000;
  if (seconds < 60) return `${seconds.toFixed(1)}s`;
  const whole = Math.floor(seconds);
  const [major, unit, minor, next] =
    whole >= 86400
      ? [Math.floor(whole / 86400), "d", Math.floor(whole / 3600) % 24, "h"]
      : whole >= 3600
        ? [Math.floor(whole / 3600), "h", Math.floor(whole / 60) % 60, "m"]
        : [Math.floor(whole / 60), "m", whole % 60, "s"];
  return `${major}${unit}${minor ? ` ${minor}${next}` : ""}`;
}
