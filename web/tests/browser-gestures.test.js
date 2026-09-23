import test from "node:test";
import assert from "node:assert/strict";
import {
  acceleratedPointerDelta,
  BROWSER_GESTURE,
  constrainView,
  panView,
  pinchView,
} from "../src/features/browser/gestures.ts";

test("browser view stays inside the display at every scale", () => {
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
      scale: BROWSER_GESTURE.maximumViewScale,
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

test("trackpad acceleration is bounded and preserves direction", () => {
  assert.deepEqual(acceleratedPointerDelta(0, 0, 16), { x: 0, y: 0 });
  const normal = acceleratedPointerDelta(8, -4, 16);
  assert(normal.x > 0);
  assert(normal.y < 0);
  const fast = acceleratedPointerDelta(1_000, 0, 1);
  assert.equal(fast.x, 1_000 * BROWSER_GESTURE.pointerMaximumGain);
});

test("cursor follow pans only at viewport edges and cannot leave the desktop", async () => {
  const { followPointer } = await import("../src/features/browser/gestures.ts");
  const view = { scale: 3, x: -200, y: -100 };
  assert.deepEqual(followPointer(view, 400, 250, { x: 200, y: 125 }), view);
  const next = followPointer(view, 400, 250, { x: 410, y: -10 });
  assert.equal(next.x, -234);
  assert.equal(next.y, -66);
  assert.deepEqual(
    followPointer({ scale: 1, x: 0, y: 0 }, 400, 250, { x: 500, y: -100 }),
    { scale: 1, x: 0, y: 0 },
  );
});
