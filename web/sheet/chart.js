// A chart's kinds as its menu names them, and what one put in is.
export const CHARTS = {
  bar: "Bars",
  stacked: "Stacked bars",
  line: "Lines",
  pie: "Pie",
};
export const CHART = { "data-chart": "bar", "data-size": "6x12" };

// Round steps that take in `low`, `high` and zero.
export function scaleOf(low, high) {
  const min = Math.min(0, low);
  const max = Math.max(0, high);
  const rough = (max - min || 1) / 4;
  const power = 10 ** Math.floor(Math.log10(rough));
  const step = [1, 2, 5, 10]
    .map((times) => times * power)
    .find((size) => size >= rough);
  const from = Math.floor(min / step) * step;
  const count = Math.max(1, Math.ceil((max - from) / step));
  return Array.from({ length: count + 1 }, (_, at) => from + at * step);
}

const rounded = (n) => Math.round(n * 100) / 100;
// Digits that count, not decimals: a scale of 0.02 steps reads as one.
const short = new Intl.NumberFormat("en", {
  notation: "compact",
  maximumSignificantDigits: 3,
});
// Room for the scale's numbers at the left and the labels under the marks.
const PAD = { left: 44, bottom: 20, top: 8, right: 8 };

// `series` hold a number or nothing per label. A pie is its first series,
// what is not above zero left out.
export function chartShapes(type, series, labels, { width, height }) {
  if (type === "pie") {
    const values = labels.map((_, at) =>
      Math.max(0, series[0].values[at] ?? 0),
    );
    const total = values.reduce((sum, value) => sum + value, 0) || 1;
    const radius = Math.max(0, Math.min(width, height) / 2 - PAD.top);
    const point = (share) => [
      rounded(width / 2 + radius * Math.sin(2 * Math.PI * share)),
      rounded(height / 2 - radius * Math.cos(2 * Math.PI * share)),
    ];
    let done = 0;
    return {
      slices: values.map((value, at) => {
        const from = done;
        done += value / total;
        // Two arcs: one cannot end where it starts, as a whole circle's does.
        const arc = (to) => `A${radius} ${radius} 0 0 1 ${point(to).join(" ")}`;
        const middle = (from + done) / 2;
        return {
          at,
          path: `M${width / 2} ${height / 2}L${point(from).join(" ")}${arc(middle)}${arc(done)}Z`,
        };
      }),
    };
  }
  const stacked = type === "stacked";
  const sums = (sign) =>
    labels.map((_, at) =>
      series.reduce(
        (sum, { values }) => sum + Math.max(0, sign * (values[at] ?? 0)),
        0,
      ),
    );
  const all = stacked
    ? [...sums(1), ...sums(-1).map((sum) => -sum)]
    : series.flatMap(({ values }) => values.filter((value) => value != null));
  const ticks = scaleOf(Math.min(...all), Math.max(...all));
  const plot = {
    left: PAD.left,
    top: PAD.top,
    width: Math.max(1, width - PAD.left - PAD.right),
    height: Math.max(1, height - PAD.top - PAD.bottom),
  };
  const [from, to] = [ticks[0], ticks.at(-1)];
  const y = (value) => plot.top + ((to - value) / (to - from)) * plot.height;
  const band = plot.width / Math.max(1, labels.length);
  const middle = (at) => plot.left + band * (at + 0.5);
  const bar = (at, series, low, high, across, of) => ({
    series,
    x: middle(at) - band * 0.4 + ((band * 0.8) / of) * across,
    y: y(Math.max(low, high)),
    width: (band * 0.8) / of,
    height: Math.abs(y(low) - y(high)),
  });
  // Where the next stacked bar of each label starts, above and below zero.
  const stood = labels.map(() => [0, 0]);
  return {
    plot,
    ticks: ticks.map((value) => ({
      value,
      y: y(value),
      text: short.format(value),
    })),
    labels: labels.map((text, at) => ({ text, x: middle(at) })),
    bars:
      type === "line"
        ? []
        : series.flatMap(({ values }, n) =>
            labels.flatMap((_, at) => {
              const value = values[at];
              if (value == null) return [];
              if (!stacked) return [bar(at, n, 0, value, n, series.length)];
              const side = value < 0 ? 1 : 0;
              const base = stood[at][side];
              stood[at][side] += value;
              return [bar(at, n, base, base + value, 0, 1)];
            }),
          ),
    lines:
      type === "line"
        ? series.map(({ values }, n) => ({
            series: n,
            points: values
              .map(
                (value, at) =>
                  value != null &&
                  `${rounded(middle(at))},${rounded(y(value))}`,
              )
              .filter(Boolean)
              .join(" "),
          }))
        : [],
  };
}
