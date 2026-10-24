import { test } from "node:test";
import assert from "node:assert/strict";
import { normalizeZoom, zoomSteps } from "../src/shared/layout.ts";

test("a stored zoom snaps to a step the picker shows", () => {
  for (const step of zoomSteps) assert.equal(normalizeZoom(step), step);
  assert.equal(normalizeZoom(85), 80);
  assert.equal(normalizeZoom(87), 90);
  assert.equal(normalizeZoom(10), 50);
  assert.equal(normalizeZoom(400), 200);
  assert.equal(normalizeZoom(0), 100);
});
