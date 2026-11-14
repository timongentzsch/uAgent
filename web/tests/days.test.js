import { test } from "node:test";
import assert from "node:assert/strict";
import { DAYS, dayOf } from "../src/features/sidebar/days.ts";

const now = new Date(2026, 10, 12, 9, 30).getTime();
const at = (days, hours = 0, minutes = 0) =>
  new Date(2026, 10, 12 - days, hours, minutes).getTime();
const day = (time) => DAYS[dayOf(time, now)];

test("days split at local midnight", () => {
  assert.equal(day(at(0)), "Today");
  assert.equal(day(now), "Today");
  assert.equal(day(at(0) - 1), "Yesterday");
  assert.equal(day(at(1, 23, 59)), "Yesterday");
  assert.equal(day(at(1)), "Yesterday");
});

test("this week is the five days before yesterday, then earlier", () => {
  assert.equal(day(at(2, 12)), "This week");
  assert.equal(day(at(6)), "This week");
  assert.equal(day(at(6) - 1), "Earlier");
  assert.equal(day(at(40)), "Earlier");
  assert.equal(day(undefined), "Earlier");
});
