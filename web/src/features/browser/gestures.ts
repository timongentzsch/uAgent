import type { Point } from "../../shared/zoom.ts";

// The touch vocabulary in one place, in CSS pixels of the device: a tap
// clicks, a drag scrolls, a hold then drag holds the left button, a
// two-finger tap right-clicks and a pinch zooms this device's view.
export const BROWSER_GESTURE = Object.freeze({
  movementSlopPx: 8,
  holdMs: 400,
  tapMs: 300,
  // One wheel notch per this much finger travel; one notch scrolls Chrome
  // about 100px, so a notch per 40px feels a little faster than the finger.
  wheelStepPx: 40,
});

// X11 wheel buttons as RFB mask bits: 4 up, 5 down, 6 left, 7 right.
export const WHEEL = {
  up: 1 << 3,
  down: 1 << 4,
  left: 1 << 5,
  right: 1 << 6,
};

// Turns accumulated finger travel into whole wheel notches, natural-scroll
// style (the content follows the finger), and returns the unspent travel.
export function wheelNotches(travel: Point) {
  const step = BROWSER_GESTURE.wheelStepPx;
  const whole = (value: number) => Math.trunc(value / step);
  const across = whole(travel.x);
  const along = whole(travel.y);
  const masks: number[] = [];
  for (let i = 0; i < Math.abs(along); i++)
    masks.push(along < 0 ? WHEEL.down : WHEEL.up);
  for (let i = 0; i < Math.abs(across); i++)
    masks.push(across < 0 ? WHEEL.right : WHEEL.left);
  return {
    masks,
    rest: { x: travel.x - across * step, y: travel.y - along * step },
  };
}
