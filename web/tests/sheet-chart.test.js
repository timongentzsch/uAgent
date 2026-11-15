import test from "node:test";
import assert from "node:assert/strict";
import { chartShapes, scaleOf } from "../sheet/chart.js";

test("a scale runs in a few round steps over the values and zero", () => {
  assert.deepEqual(scaleOf(3, 87), [0, 50, 100]);
  assert.deepEqual(scaleOf(-30, 45), [-40, -20, 0, 20, 40, 60]);
  assert.deepEqual(scaleOf(0.2, 0.9), [0, 0.5, 1]);
  assert.deepEqual(scaleOf(1200, 8400), [0, 5000, 10000]);
  assert.deepEqual(scaleOf(5, 5), [0, 2, 4, 6]);
  assert.equal(scaleOf(0, 0).at(-1) > 0, true);
});

test("the scale's numbers keep the digits that tell them apart", () => {
  const texts = (low, high) =>
    chartShapes("bar", [{ values: [low, high] }], ["a", "b"], size).ticks.map(
      ({ text }) => text,
    );
  assert.deepEqual(texts(0.01, 0.07), ["0", "0.02", "0.04", "0.06", "0.08"]);
  assert.deepEqual(texts(1200, 8400), ["0", "5K", "10K"]);
  assert.deepEqual(texts(-1.5, 2.5), ["-2", "-1", "0", "1", "2", "3"]);
});

const size = { width: 252, height: 128 };
const series = [{ values: [50, -25] }, { values: [100, undefined] }];

test("bars stand on zero, each as tall as its value, side by side for each label", () => {
  const { plot, ticks, labels, bars, lines } = chartShapes(
    "bar",
    series,
    ["a", "b"],
    size,
  );
  assert.deepEqual(plot, { left: 44, top: 8, width: 200, height: 100 });
  assert.deepEqual(
    ticks.map(({ value }) => value),
    [-50, 0, 50, 100],
  );
  const zero = ticks[1].y;
  assert.equal(Math.round(zero), 75);
  assert.deepEqual(labels, [
    { text: "a", x: 94 },
    { text: "b", x: 194 },
  ]);
  assert.deepEqual(lines, []);
  // 50 of 150 is a third of the plot; a value that is missing draws nothing.
  const round = ({ series, x, y, width, height }) =>
    [series, x, y, width, height].map(Math.round);
  assert.deepEqual(bars.map(round), [
    [0, 54, 41, 40, 33],
    [0, 154, 75, 40, 17],
    [1, 94, 8, 40, 67],
  ]);
});

test("stacked bars stand on each other, what is below zero under it; lines run through the values", () => {
  const stacked = chartShapes(
    "stacked",
    [{ values: [50, -25] }, { values: [100, -25] }],
    ["a", "b"],
    size,
  );
  assert.deepEqual(
    stacked.ticks.map(({ value }) => value),
    [-50, 0, 50, 100, 150],
  );
  const tops = stacked.bars.map(({ y, height }) => [
    Math.round(y),
    Math.round(height),
  ]);
  assert.deepEqual(tops, [
    [58, 25],
    [83, 13],
    [8, 50],
    [96, 13],
  ]);
  const { bars, lines } = chartShapes("line", series, ["a", "b"], size);
  assert.deepEqual(bars, []);
  assert.deepEqual(
    lines.map(({ points }) => points.split(" ").length),
    [2, 1],
  );
});

test("a pie's slices are its first series' shares of one circle", () => {
  const { slices } = chartShapes(
    "pie",
    [{ values: [1, 1, -3, undefined] }],
    ["a", "b", "c", "d"],
    { width: 116, height: 116 },
  );
  assert.equal(slices.length, 4);
  // Half the circle each: from the top, round the right, to the bottom.
  assert.equal(
    slices[0].path,
    "M58 58L58 8A50 50 0 0 1 108 58A50 50 0 0 1 58 108Z",
  );
  assert.match(
    slices[1].path,
    /^M58 58L58 108A50 50 0 0 1 8 58A50 50 0 0 1 58 8/,
  );
});
