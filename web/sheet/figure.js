// A tab's charts, drawn as SVG over its grid from what the engine makes of
// the cells they name.

import { CHART, CHARTS, chartShapes } from "./chart.js";
import { make } from "./grid.js";
import { openMenu } from "./menu.js";
import { boxNamed, CELL, cellName } from "./text.js";

// A chart covers no more cells than this, whatever its file says.
const MOST = 200;
// The stylesheet has this many series colours; further series start over.
const COLOURS = 5;
// Labels closer than this would run into each other: some are left out.
const LABEL_ROOM = 48;

const shape = (name, attrs = {}, text = "") => {
  const element = document.createElementNS("http://www.w3.org/2000/svg", name);
  for (const [key, value] of Object.entries(attrs))
    element.setAttribute(key, value);
  element.textContent = text;
  return element;
};

// The cell whose edge is nearest to a place: `at` finds the cell it lies in,
// `start` where a cell starts.
const nearest = (at, start, place) => {
  const cell = at(place);
  return start(cell + 1) - place < place - start(cell) ? cell + 1 : cell;
};
const units = (length, unit) =>
  Math.max(1, Math.min(MOST, Math.round(length / unit)));

// Mouse only: a touch that drags scrolls the page. A drag on the chart moves
// it; one on a corner (`data-corner`: "nw", "se") moves that corner.
function drag(layer, sheet, change) {
  layer.addEventListener("pointerdown", (event) => {
    const figure = event.target.closest("figure");
    if (!figure || event.button || event.pointerType !== "mouse") return;
    if (event.target.closest("button")) return;
    const corner = event.target.dataset.corner ?? "";
    const [north, west] = [corner.includes("n"), corner.includes("w")];
    const from = figure.getBoundingClientRect();
    const by = (to) => [to.clientX - event.clientX, to.clientY - event.clientY];
    const move = (to) => {
      const [x, y] = by(to);
      const box = {
        left: from.left + scrollX + (corner && !west ? 0 : x),
        top: from.top + scrollY + (corner && !north ? 0 : y),
        width: from.width + (corner ? (west ? -x : x) : 0),
        height: from.height + (corner ? (north ? -y : y) : 0),
      };
      for (const [side, length] of Object.entries(box))
        figure.style[side] = `${length}px`;
    };
    figure.setPointerCapture(event.pointerId);
    figure.addEventListener("pointermove", move);
    figure.addEventListener(
      "pointerup",
      (to) => {
        figure.removeEventListener("pointermove", move);
        if (by(to).every((moved) => Math.abs(moved) < 4)) return;
        const { x, y, columnAt, rowAt } = sheet.layout();
        const now = figure.getBoundingClientRect();
        // A move changes where it sits, a corner how much it covers; the
        // corners at its top and left change both.
        const next = {
          ...(corner && {
            "data-size": `${units(now.width, CELL.width)}x${units(now.height, CELL.height)}`,
          }),
          ...(corner !== "se" && {
            "data-at": cellName({
              column: nearest(columnAt, x, now.left + scrollX),
              row: nearest(rowAt, y, now.top + scrollY),
            }),
          }),
        };
        change([...layer.children].indexOf(figure), (chart) => [
          { ...chart, attrs: { ...chart.attrs, ...next } },
        ]);
      },
      { once: true },
    );
  });
}

function drawn(type, { labels, series }, size) {
  const { plot, ticks, bars, lines, slices, ...shapes } = chartShapes(
    type,
    series,
    labels,
    size,
  );
  if (slices)
    return slices.map(({ at, path }) =>
      shape("path", { d: path, "data-series": at % COLOURS }),
    );
  const every = Math.ceil(LABEL_ROOM / (plot.width / labels.length));
  return [
    ...ticks.flatMap(({ value, y, text }) => [
      shape("line", {
        x1: plot.left,
        x2: plot.left + plot.width,
        y1: y,
        y2: y,
        "data-zero": value === 0,
      }),
      shape("text", { x: plot.left - 6, y, "data-axis": "y" }, text),
    ]),
    ...shapes.labels
      .filter((_, at) => at % every === 0)
      .map(({ text, x }) =>
        shape(
          "text",
          { x, y: plot.top + plot.height + 14, "data-axis": "x" },
          text,
        ),
      ),
    ...bars.map(({ series, ...box }) =>
      shape("rect", { ...box, "data-series": series % COLOURS }),
    ),
    ...lines.map(({ series, points }) =>
      shape("polyline", { points, "data-series": series % COLOURS }),
    ),
  ];
}

export function listenCharts(sheet) {
  const layer = document.body.appendChild(
    make("div", { className: "sheet-charts" }),
  );
  const change = (index, next) =>
    sheet.did(
      sheet.book.setCharts(sheet.at, (charts) =>
        charts.flatMap((chart, at) => (at === index ? next(chart) : [chart])),
      ),
    );
  drag(layer, sheet, change);
  // The chart picked, of the tab it was picked on: it shows its corners and
  // its tools, and the cells it reads are tinted.
  let picked = { tab: -1, chart: -1 };
  const pick = (chart) => {
    picked = { tab: sheet.at, chart };
    show();
  };
  layer.addEventListener("click", (event) => {
    const index = [...layer.children].indexOf(event.target.closest("figure"));
    const tool = event.target.closest("button")?.name;
    if (index < 0) return;
    if (tool === "delete") drop(index);
    else if (tool === "edit")
      openMenu(
        { x: event.clientX, y: event.clientY },
        Object.entries(CHARTS).map(([type, label]) => ({
          label,
          run: () =>
            change(index, (chart) => [
              { ...chart, attrs: { ...chart.attrs, "data-chart": type } },
            ]),
        })),
        sheet.hold,
      );
    else if (index !== picked.chart) pick(index);
  });
  // A press on a cell lets go of it.
  const free = () => picked.chart >= 0 && pick(-1);
  addEventListener("mousedown", (event) => {
    if (!event.target.closest?.(".sheet-chart, .sheet-menu")) free();
  });
  // No chart is picked once one is taken out: the next has its place.
  const drop = (index) => {
    picked = { tab: -1, chart: -1 };
    change(index, () => []);
  };
  // What looks picked is what a key acts on: Delete takes the chart out,
  // before the cells hear it. Any other key lets go and is the cells'.
  addEventListener(
    "keydown",
    (event) => {
      if (picked.chart < 0 || picked.tab !== sheet.at) return;
      if (event.key !== "Delete" && event.key !== "Backspace") return free();
      event.preventDefault();
      event.stopPropagation();
      drop(picked.chart);
    },
    true,
  );

  const show = () => {
    const { x, y } = sheet.layout();
    const chosen = picked.tab === sheet.at ? picked.chart : -1;
    sheet.reads(boxNamed(sheet.tab().charts[chosen]?.attrs["data-range"]));
    const figures = sheet.tab().charts.map(({ attrs, caption }, index) => {
      // Unsaid, beside the grid.
      const on = boxNamed(attrs["data-at"]) ?? {
        top: 1,
        left: sheet.tab().columns.length + 2,
      };
      const [wide, high] = (attrs["data-size"] ?? CHART["data-size"])
        .split("x")
        .map((cells) => units(Number(cells) || 1, 1));
      const type = attrs["data-chart"];
      const data = sheet.book.chart(sheet.at, index);
      const kind = Object.hasOwn(CHARTS, type) ? CHARTS[type] : "Chart";
      const name = caption || `${kind} of ${attrs["data-range"]}`;
      const picture = shape("svg", { role: "img", "aria-label": name });
      picture.append(shape("title", {}, name));
      const figure = make("figure", { className: "sheet-chart" }, [
        make("figcaption", { textContent: caption }),
        data.error
          ? make("p", { textContent: `This chart ${data.error}.` })
          : picture,
      ]);
      if (index === chosen) {
        figure.dataset.picked = "";
        figure.append(
          make(
            "span",
            { className: "sheet-chart-tools" },
            ["Edit", "Delete"].map((label) =>
              make("button", {
                type: "button",
                name: label.toLowerCase(),
                textContent: label,
                ariaLabel: `${label} chart: ${name}`,
              }),
            ),
          ),
          ...["nw", "ne", "sw", "se"].map((corner) => {
            const handle = make("span", { className: "sheet-size" });
            handle.dataset.corner = corner;
            return handle;
          }),
        );
      }
      Object.assign(figure.style, {
        left: `${x(on.left)}px`,
        top: `${y(on.top)}px`,
        width: `${wide * CELL.width}px`,
        height: `${high * CELL.height}px`,
      });
      const named =
        type === "pie" ? data.labels : data.series?.map(({ name }) => name);
      if (named?.length > 1)
        figure.append(
          make(
            "ul",
            {},
            named.map((text, at) => {
              const item = make("li", { textContent: text });
              item.dataset.series = at % COLOURS;
              return item;
            }),
          ),
        );
      return { figure, picture, type, data };
    });
    layer.replaceChildren(...figures.map(({ figure }) => figure));
    // Drawn once laid out: the picture takes the room the figure leaves it.
    for (const { picture, type, data } of figures)
      if (!data.error)
        picture.append(
          ...drawn(type, data, {
            width: picture.clientWidth,
            height: picture.clientHeight,
          }),
        );
  };
  return show;
}
