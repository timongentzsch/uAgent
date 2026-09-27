import test from "node:test";
import assert from "node:assert/strict";
import {
  BROWSER_GESTURE,
  wheelNotches,
} from "../src/features/browser/gestures.ts";
import {
  MAXIMUM_ZOOM,
  constrainView,
  panView,
  pinchView,
} from "../src/shared/zoom.ts";

test("a zoomed view stays inside its surface at every scale", () => {
  assert.deepEqual(constrainView({ scale: 0.5, x: 20, y: 20 }, 400, 250), {
    scale: 1,
    x: 0,
    y: 0,
  });
  assert.deepEqual(constrainView({ scale: 2, x: -900, y: 40 }, 400, 250), {
    scale: 2,
    x: -400,
    y: 0,
  });
  assert.deepEqual(
    constrainView({ scale: 20, x: -9_000, y: -9_000 }, 400, 250),
    {
      scale: MAXIMUM_ZOOM,
      x: -1_600,
      y: -1_000,
    },
  );
});

test("pinch keeps its content anchor and one-finger pan remains bounded", () => {
  const zoomed = pinchView(
    { scale: 1, x: 0, y: 0 },
    400,
    250,
    { x: 200, y: 125 },
    { x: 200, y: 125 },
    2,
  );
  assert.deepEqual(zoomed, { scale: 2, x: -200, y: -125 });
  assert.deepEqual(panView(zoomed, 400, 250, 500, -500), {
    scale: 2,
    x: 0,
    y: -250,
  });
});

test("finger travel becomes natural-scroll wheel notches", () => {
  const step = BROWSER_GESTURE.wheelStepPx;
  // Finger up: the content follows it, so Chrome scrolls down (button 5).
  assert.deepEqual(wheelNotches({ x: 0, y: -2.5 * step }), {
    masks: [1 << 4, 1 << 4],
    rest: { x: 0, y: -0.5 * step },
  });
  // Finger down and left: scroll up (button 4) and right (button 7).
  assert.deepEqual(wheelNotches({ x: -step, y: step }).masks, [1 << 3, 1 << 6]);
  // Under one step nothing is sent and the travel carries over.
  assert.deepEqual(wheelNotches({ x: 3, y: -5 }), {
    masks: [],
    rest: { x: 3, y: -5 },
  });
});
