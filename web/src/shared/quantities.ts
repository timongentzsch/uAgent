// Human-facing quantities only. Wire data, config values and raw captures stay exact.
function scaled(value: number, units: string[], separator = "") {
  let unit = 0;
  while (Math.abs(value) >= 999.95 && unit < units.length - 1) {
    value /= 1000;
    unit++;
  }
  return `${Number(value.toFixed(1))}${separator}${units[unit]}`;
}
export const count = (value?: number) =>
  value !== undefined && Number.isFinite(value) && value >= 0
    ? scaled(value, ["", "k", "M", "B", "T", "P", "E"])
    : "Not recorded";
export const bytes = (value: number) =>
  Number.isFinite(value) && value >= 0
    ? scaled(value, ["B", "kB", "MB", "GB", "TB", "PB", "EB"], " ")
    : "Not recorded";
// Same precision as the terminal's FmtCost: cents above a dollar.
export const cost = (value: number) => `$${value.toFixed(value < 1 ? 4 : 2)}`;
