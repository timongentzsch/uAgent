import { test } from "node:test";
import assert from "node:assert/strict";
import {
  formatMoment,
  formatRelative,
  normalizeTimePrefs,
} from "../src/shared/time.ts";

// Expectations are built with the same Intl APIs, so the suite holds in any
// locale; what it pins is which parts each style shows.
const now = new Date(2026, 8, 24, 15, 0);
const at = (days, hours = 14, minutes = 32) =>
  new Date(2026, 8, 24 - days, hours, minutes);
const clock24 = (date) =>
  new Intl.DateTimeFormat(undefined, {
    hour: "numeric",
    minute: "2-digit",
    hourCycle: "h23",
  }).format(date);

test("smart stamps grow from a clock to a date as they age", () => {
  const prefs = { clock: "24", style: "smart" };
  assert.equal(formatMoment(at(0), prefs, now), clock24(at(0)));
  const yesterday = formatMoment(at(1), prefs, now);
  assert.ok(yesterday.endsWith(clock24(at(1))), yesterday);
  assert.notEqual(yesterday, clock24(at(1)));
  const week = formatMoment(at(3), prefs, now);
  assert.ok(
    week.includes(
      new Intl.DateTimeFormat(undefined, { weekday: "short" }).format(at(3)),
    ),
    week,
  );
  const older = formatMoment(new Date(2024, 0, 2), prefs, now);
  assert.ok(older.includes("2024"), older);
});

test("the clock preference forces 12 or 24 hours", () => {
  const evening = at(0, 20, 5);
  assert.ok(
    formatMoment(evening, { clock: "24", style: "smart" }, now).includes("20"),
  );
  assert.ok(
    !formatMoment(evening, { clock: "12", style: "smart" }, now).includes("20"),
  );
});

test("relative and absolute styles", () => {
  const rtf = new Intl.RelativeTimeFormat(undefined, { numeric: "auto" });
  assert.equal(
    formatRelative(new Date(now - 20_000), now),
    rtf.format(0, "second"),
  );
  assert.equal(
    formatRelative(new Date(now - 5 * 60_000), now),
    rtf.format(-5, "minute"),
  );
  assert.equal(
    formatRelative(new Date(now - 3 * 86_400_000), now),
    rtf.format(-3, "day"),
  );
  const absolute = formatMoment(at(0), { clock: "24", style: "absolute" }, now);
  assert.ok(absolute.includes("2026") && absolute.includes(clock24(at(0))));
});

test("stored preferences are normalized", () => {
  assert.deepEqual(normalizeTimePrefs({ clock: "25", style: "smart" }), {
    clock: "system",
    style: "smart",
  });
  assert.deepEqual(normalizeTimePrefs(null), {
    clock: "system",
    style: "smart",
  });
});
